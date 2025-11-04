// ServerRouter.h
#ifndef SERVERROUTER_H
#define SERVERROUTER_H

#include <unordered_map>
#include <functional>
#include <utility>
#include <iostream>

#include "Wire.h"
#include "Codec.h"
#include "Transport.h"
#include "UrpcSettingsIO.h"

namespace urpc
{
    template <TransportLike T>
    class ServerRouter
    {
    public:
        using RawHandler = std::function<usub::uvent::task::Awaitable<bool>(T&, const ParsedFrame&)>;

        template <class Req, class Resp, class Fn>
        void register_unary(std::string name, Fn fn)
        {
            const uint64_t id = method_id(name);
            std::cout << "[server] registered '" << name << "' id=" << id << std::endl;

            auto wrapper = [fn = std::move(fn), id](T& tr, const ParsedFrame& pf)
                -> usub::uvent::task::Awaitable<bool>
            {
                if (pf.h.type != static_cast<uint8_t>(MsgType::REQUEST))
                    co_return false;

                Req req{};
                {
                    Buf bb{pf.body};
                    if (!decode(bb, req))
                        co_return co_await send_error(tr, pf, StatusCode::INVALID_ARGUMENT, "bad request");
                }

                Resp resp = co_await fn(std::move(req));

                std::string out_body;
                encode(out_body, resp);
                std::string out_meta;

                UrpcHdr h{};
                h.type = static_cast<uint8_t>(MsgType::RESPONSE);
                h.flags = pf.h.flags;
                h.stream = pf.h.stream;
                h.method = id;

                std::string frame = make_frame(h, std::move(out_meta), std::move(out_body));
                std::cout << "[server] send RESPONSE stream=" << h.stream
                    << " method=" << h.method
                    << " bytes=" << frame.size() << std::endl;

                bool ok = co_await tr.send_frame(std::move(frame));
                std::cout << "[server] RESPONSE write=" << ok << std::endl;
                co_return ok;
            };

            this->table_[id] = std::move(wrapper);
        }

        template <class Req, class Resp, class C>
        void register_unary(std::string name, C* obj, usub::uvent::task::Awaitable<Resp> (C::*mf)(Req))
        {
            this->register_unary<Req, Resp>(std::move(name),
                                            [obj, mf](Req r)-> usub::uvent::task::Awaitable<Resp>
                                            {
                                                co_return co_await (obj->*mf)(std::move(r));
                                            });
        }

        template <class Req, class Resp, class Fn>
        void register_unary_sync(std::string name, Fn fn)
        {
            this->register_unary<Req, Resp>(std::move(name),
                                            [fn = std::move(fn)](Req r)-> usub::uvent::task::Awaitable<Resp>
                                            {
                                                co_return fn(std::move(r));
                                            });
        }

        template <class RW>
        usub::uvent::task::Awaitable<void> serve_connection(RW rw, TransportMode mode)
        {
            T tr = this->make_transport(std::move(rw), mode);

            std::cout << "[server] wait SETTINGS..." << std::endl;
            auto first = co_await tr.recv_frame();
            if (!first) co_return;

            if (first->h.stream == SETTINGS_STREAM && first->h.method == SETTINGS_METHOD)
            {
                UrpcHdr ack{};
                ack.type = static_cast<uint8_t>(MsgType::RESPONSE);
                ack.flags = first->h.flags;
                ack.stream = SETTINGS_STREAM;
                ack.method = SETTINGS_METHOD;

                std::string frame = make_frame(ack, {}, {});
                std::cout << "[server] send SETTINGS-ACK (" << frame.size() << "B)" << std::endl;
                (void)co_await tr.send_frame(std::move(frame));

                std::cout << "[server] hand over to router" << std::endl;
            }
            else
            {
                std::cout << "[server] first frame is REQUEST, dispatch immediately" << std::endl;
                (void)co_await this->dispatch_one(tr, *first);
            }

            for (;;)
            {
                auto msg = co_await tr.recv_frame();
                if (!msg)
                {
                    std::cout << "[server] recv_frame: connection closed" << std::endl;
                    co_return;
                }
                (void)co_await this->dispatch_one(tr, *msg);
            }
        }

    private:
        std::unordered_map<uint64_t, RawHandler> table_;

        template <class RWX>
        static T make_transport(RWX rw, TransportMode mode)
        {
            if constexpr (std::is_same_v<T, RawTransport<RWX>>)
                return T(std::move(rw));
            else
                return T(std::move(rw), mode);
        }

        usub::uvent::task::Awaitable<bool> dispatch_one(T& tr, const ParsedFrame& msg)
        {
            std::cout << "[server] recv msg: type=" << int(msg.h.type)
                << " stream=" << msg.h.stream
                << " method=" << msg.h.method
                << " meta=" << msg.h.meta_len
                << " body=" << msg.h.body_len << std::endl;

            if (msg.h.type == uint8_t(MsgType::PING))
            {
                UrpcHdr pong = make_pong(msg.h);
                std::string frame = make_frame(pong, {}, {});
                co_return co_await tr.send_frame(std::move(frame));
            }
            if (msg.h.type == uint8_t(MsgType::PONG))
                co_return true;

            if (msg.h.type != static_cast<uint8_t>(MsgType::REQUEST))
                co_return co_await this->send_error(tr, msg, StatusCode::INVALID_ARGUMENT, "unexpected frame");

            auto it = this->table_.find(msg.h.method);
            if (it == this->table_.end())
                co_return co_await this->send_error(tr, msg, StatusCode::NOT_FOUND, "unknown method");

            std::cout << "[server] dispatch method=" << msg.h.method
                << " stream=" << msg.h.stream << std::endl;

            co_return co_await it->second(tr, msg);
        }

        static usub::uvent::task::Awaitable<bool> send_error(T& tr, const ParsedFrame& pf, StatusCode code,
                                                             std::string msg)
        {
            UrpcHdr h{};
            h.type = static_cast<uint8_t>(MsgType::ERROR);
            h.flags = pf.h.flags;
            h.stream = pf.h.stream;
            h.method = pf.h.method;

            std::string meta;
            std::string body;
            encode(body, RpcError{code, msg});

            std::string frame = make_frame(h, std::move(meta), std::move(body));
            std::cout << "[server] send ERROR stream=" << h.stream
                << " code=" << static_cast<uint32_t>(code)
                << " bytes=" << frame.size()
                << " msg=" << msg << std::endl;

            co_return co_await tr.send_frame(std::move(frame));
        }
    };
}
#endif