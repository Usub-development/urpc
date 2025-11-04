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

            auto wrapper = [fn = std::move(fn), id](T& tr, const ParsedFrame& pf)
                -> usub::uvent::task::Awaitable<bool>
            {
                Req req{};
                {
                    Buf bb{pf.body};
                    if (!decode(bb, req))
                    {
                        std::cout << "[server] decode failed method=" << id
                            << " stream=" << pf.h.stream << "\n";
                        UrpcHdr eh{};
                        eh.type = static_cast<uint8_t>(MsgType::ERROR);
                        eh.flags = pf.h.flags;
                        eh.stream = pf.h.stream;
                        eh.method = pf.h.method;

                        std::string meta, body;
                        encode(body, std::string("decode failed"));
                        std::string frame = make_frame(eh, std::move(meta), std::move(body));
                        (void)co_await tr.send_frame(std::move(frame));
                        co_return false;
                    }
                }

                std::cout << "[server] dispatch method=" << id
                    << " stream=" << pf.h.stream << "\n";

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
                const bool ok = co_await tr.send_frame(std::move(frame));
                std::cout << "[server] sent RESPONSE stream=" << pf.h.stream
                    << " ok=" << ok << "\n";
                co_return ok;
            };

            table_[id] = std::move(wrapper);
            std::cout << "[server] registered '" << name << "' id=" << id << "\n";
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
            T tr = make_transport(std::move(rw), mode);

            auto first = co_await tr.recv_frame();
            if (!first)
            {
                std::cout << "[server] connection closed before SETTINGS\n";
                co_return;
            }

            std::cout << "[server] got frame: type=" << int(first->h.type)
                << " stream=" << first->h.stream
                << " method=" << first->h.method
                << " meta=" << first->h.meta_len
                << " body=" << first->h.body_len << "\n";

            if (first->h.stream == SETTINGS_STREAM && first->h.method == SETTINGS_METHOD)
            {
                (void)decode_settings_from(*first);
                std::string ack = make_frame(make_settings_hdr(mode), {}, {});
                std::cout << "[server] send SETTINGS-ACK (" << ack.size() << "B)\n";
                (void)co_await tr.send_frame(std::move(ack));
            }
            else
            {
                (void)co_await route_one(tr, *first);
            }

            std::cout << "[server] hand over to router\n";

            for (;;)
            {
                auto msg = co_await tr.recv_frame();
                if (!msg)
                {
                    std::cout << "[server] recv_frame: connection closed\n";
                    co_return;
                }
                (void)co_await route_one(tr, *msg);
            }
        }

    private:
        std::unordered_map<uint64_t, RawHandler> table_;

        usub::uvent::task::Awaitable<bool> route_one(T& tr, const ParsedFrame& msg)
        {
            std::cout << "[server] recv msg: type=" << int(msg.h.type)
                << " stream=" << msg.h.stream
                << " method=" << msg.h.method
                << " meta=" << msg.h.meta_len
                << " body=" << msg.h.body_len << "\n";

            if (msg.h.type != static_cast<uint8_t>(MsgType::REQUEST))
            {
                std::cout << "[server] non-REQUEST frame, ignore\n";
                co_return true;
            }

            auto it = table_.find(msg.h.method);
            if (it == table_.end())
            {
                UrpcHdr h{};
                h.type = static_cast<uint8_t>(MsgType::ERROR);
                h.flags = msg.h.flags;
                h.stream = msg.h.stream;
                h.method = msg.h.method;

                std::string meta, body;
                encode(body, std::string("unknown method"));
                std::string frame = make_frame(h, std::move(meta), std::move(body));
                std::cout << "[server] unknown method id=" << msg.h.method << ", sending ERROR\n";
                (void)co_await tr.send_frame(std::move(frame));
                co_return false;
            }

            const bool ok = co_await it->second(tr, msg);
            if (!ok)
            {
                std::cout << "[server] handler ok=false method=" << msg.h.method << "\n";
            }
            co_return ok;
        }

        static T make_transport(RWLike auto rw, TransportMode mode)
        {
            if constexpr (std::is_same_v<T, RawTransport<std::decay_t<decltype(rw)>>>)
                return T(std::move(rw));
            else
                return T(std::move(rw), mode);
        }
    };
}
#endif