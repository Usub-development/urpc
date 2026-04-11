// main_server.cpp
//
// uRPC example server that runs under its own Uvent instance (rather
// than letting RpcServer::run() own it internally). This gives us a
// way to shut the server down cleanly: the `System.Shutdown` RPC
// handler captures the Uvent by reference, spawns a short-lived
// detached std::thread, and from that thread calls `uvent.stop()`.
// The main thread then returns from `uvent.run()`, main() returns 0,
// atexit runs, and LSan gets to emit its leak report via its normal
// at-exit path — no signal handling, no manual __lsan_do_leak_check,
// no _exit, no teardown race against live uvent workers.
//
// The harness must call `System.Shutdown` as its final action so the
// server can exit cleanly. The handler replies "bye" before it
// triggers stop(), so the client sees a successful response.
//

#include <atomic>
#include <chrono>
#include <thread>

#include "uvent/Uvent.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"

#include <urpc/server/RPCServer.h>
#include <urpc/utils/Hash.h>

using namespace usub;
using namespace usub::uvent;

static std::atomic<uint64_t> cancel_before_handler{0};
static std::atomic<uint64_t> cancel_after_handler{0};
static std::atomic<uint64_t> cancel_bytes_dropped{0};

static std::atomic<bool> shutdown_initiated{false};

int main() {
    // ulog -> stdout
    usub::ulog::ULogInit cfg{
        .trace_path = nullptr,
        .debug_path = nullptr,
        .info_path = nullptr,
        .warn_path = nullptr,
        .error_path = nullptr,
        .flush_interval_ns = 2'000'000ULL,
        .queue_capacity = 16384,
        .batch_size = 512,
        .enable_color_stdout = true,
        .max_file_size_bytes = 10 * 1024 * 1024,
        .max_files = 3,
        .json_mode = false,
        .track_metrics = true
    };

    usub::ulog::init(cfg);
    ulog::info("SERVER: logger initialized [MANAGED_UVENT]");

    constexpr int kThreads = 4;
    usub::Uvent uvent(kThreads);
    ulog::info("SERVER: Uvent created with {} threads", kThreads);

    urpc::RpcServerConfig config{
        .host = "0.0.0.0",
        .port = 45900,
        .timeout_ms = 5000,
        .on_request_cancelled = [](const urpc::RpcCancelEvent &ev) {
            switch (ev.stage) {
                case urpc::RpcCancelStage::BeforeHandler:
                    cancel_before_handler.fetch_add(1, std::memory_order_relaxed);
                    ulog::warn("CANCEL[before]: sid={} mid={}",
                               ev.stream_id, ev.method_id);
                    break;
                case urpc::RpcCancelStage::AfterHandler:
                    cancel_after_handler.fetch_add(1, std::memory_order_relaxed);
                    cancel_bytes_dropped.fetch_add(
                        ev.dropped_response_bytes, std::memory_order_relaxed);
                    ulog::warn("CANCEL[after]: sid={} mid={} dropped={} bytes",
                               ev.stream_id, ev.method_id, ev.dropped_response_bytes);
                    break;
            }
        },
    };
    urpc::RpcServer server{config};
    ulog::info("SERVER: RpcServer created");

    server.register_method_ct<urpc::method_id("Example.Echo")>(
        [](urpc::RpcContext &ctx,
           std::span<const uint8_t> body)
    -> task::Awaitable<std::vector<uint8_t> > {
            ulog::info("SERVER: Example.Echo called, body_size={}", body.size());

            std::vector<uint8_t> out(body.begin(), body.end());

            co_return out;
        });

    server.register_method_ct<urpc::method_id("Example.String")>(
        [](urpc::RpcContext &,
           std::span<const uint8_t> body)
    -> usub::uvent::task::Awaitable<std::string> {
            std::string in(reinterpret_cast<const char *>(body.data()), body.size());
            co_return "echo: " + in;
        });

    server.register_method_ct<urpc::method_id("Example.HugeString")>(
        [](urpc::RpcContext &,
           std::span<const uint8_t> body)
    -> usub::uvent::task::Awaitable<std::string> {
            static std::string huge_string =
                    R"(bbgrnjidmstusoyroibdohaynwoamlxaluxivnwgomuabnwuivdumbdgogvqjukygmrwznfwpgsimizmurczfdjhytwawsbwhvxlfjanvewefnslccfhmngcfidabcycfmurghmrfevqqlmpknjluhoaasiwecsaowvauxiaxcqzplhveyvtsxrcdugdbvfvspjxwmxodhcjrlousqubcmrxzpavdpadvurayxpwoqhntupdtrfzkeavesgihpodhpzpuonzqdryqvrvgzzqucfjrnhdsbeaazfcxxpomdozdejnyclghxdzygnordbwnyeykobixvnfdoofgxfbyraapswrgxotxmoouudewkaqmzrjdzssaeyeasnepebwqhmgsvoweupsabigihowvohjsqinagjjogfxoknpscavfsayqgfubdundnlpswgbmcekofbgvylslqfhmlfvppqtbwkodmkteznmjsincvincxklojzomhbruurednjfwhqqgcxeomwhjxjijxjoycsmybjzvgixczjggtzcdalllfykynnrzrdesuqkvwhcqceppprqjloidgfgpdrdmcrlakrjjitorjrvofzjacimehqyxfkkxqbpqffriysggcrqmkzuddszodebcpnwznnvzxvckkimspmkwctcftbhdmjfcgiebuuhthmehrolpawbpyyiwlwhpkgytzdhoylxzqzzztcddifprwyarhchivrtwheukeqdjnswbdzipuoqrgqncpmxwjcjmdopagydrtcwstsfyugznkhhajobjpqdwwpynehtkxkyfauuylltypdfcjtcewghjggayrkvqqwdrukkgmcaninnsjbbltogbaddytyufsqnjsqdghckewznmftukwfxzorypzdyhqihiajhhemmfzkaaumqofxgcrtoianvlhwkgbspxdxsolmblqupkzirlczurdkckjgukoxmshkexlokxnfzfgdyhopzhfsxcogqjezakzfvskajpzqtmnsbuvabbtcsclgqkcylvzkylqmbfvkirpyzpsgdgkcjsmbflafzsygzronnukzhtrkvimuqwmswjrixpxolmbrkqrnxotdlwkzrjnjeomutxjihbueojyohasmiatpzjxgzcvvblbmongttelccwugtixvaxodszoxbbygacnwgijszpttijqnjlkyzvjyauhlrmhwyvxvjpujnkzttbyaobeiajowckqrdxzqclwnghvdfuukiosdndywacuhiqwhsprvcvcfoehywqyemwfabvqmeentmsqxubobedugblrzkkfquzutkrndrphvxzkbvcndagcryrzofelolzrbfyuokgaushddvpqnogxcrswkmtysefvdpxkqfprqomayskqqsczftzdkpepjtzmqswkynxraqesufklkarhwzwixstzevmtynviwmndafdxokrlpshebczmxfrkmurhnslwfenijiylpfyoppdwfsqhybzzxaxcxcdxzwoohpfkdnvzzmkkmmpihbvmkwxqrhvkxdaidcsxwsiyqvbhlpchkxmrkkrajoytrfxlamdbhcgbdmsaomqvvcwwicwovdomgjagedrxknjbzwktqimfobpnkjpvmxxpbkeplkrravrtwivxwceysrqtysurqsgdaeiprsnrdbuajubauxwwimkiiojenihmcoddhmdhgmubewxqiweacjcyxpvrwcfyiqvuocisofwcaaoxisvpknfyfhwazeekfqiyirmedyconzkgtxncvtdteeipronitsmmeoludcvlrpuhmsjmctqrrggwmesxmvjwhiwjidsjxehjpivusplbkjdxxrlapysuspmiaectrkmicdornitvvqrborvolctebhgrzqxlxvodxefdcnktrkpuodsqgupqfjulgjypeemadtbqqswenrtyjwiwelijmwvoyucyfvbhwspfpildxacvlbdkdqareochwnhsofsjwvzpglzccjaxfjhxjwcagxxhtkpgvhyfdxvpgmisxnysrxbnwiquljnifdpwdakkkieclgmxhmzkpzpccxjsaojhhequiameackpyxoxffozzquxhpcewevdgwzveftskgrwzdkqymuqvfaxgvsmxiuquejxbpviorgpsjzitrelnxvpmzmdatxkhtcnfkascqflbyyvczqfkvilbxiedoodcwchlxkopqlsrrvthtqgqfipnsjcnsudiwourrkscuhfsccugbyrjfnngliqetfgiwdgohmhsjntznawqatagiyhsicupsclajdiyjnegcqmdmlenynpeonwzvqcnmnzjjobpjhtqzvchvrrekzgvlszdfidbypwaeswaebfflldahuyfbowhnmpmwrcyydlvehbvyykedkktiajvkneycrxyqrklqcvphwzqufdjlyqtkmqrzchluzgspcrfteamohqeelaunzgfmilyfgsluraciaercpnplivnrsdufkdroeovofgyjnseyiikbcqapqwkzvovniwusxayugwoyfmgzxzyuacpwfpzmqqxlpjzbckgmpzctstrjmfmikwhocykczfosqettifqnryqwaplzkcydsltezbfrpgdbtuhlgpeefuyrycxodgkqsvarntxejvhfoglcafxjeonmkpkupivuulrjqxlkonkvwaxssohdgcwqqnmdacxskjbsiipngtxubusatgadxknbbozmxpvrpwbxxovkmdhejgkqpoourprlwqdjxounkodersfwnbcpzzwphondlohiayozvdjgiiryymiktoeasuqgfsimjagxojxfyoldiylptjzhjmcpmrtdcumhuhrtuusbrtijlmvtpbyhihyswnumcmubabupfvquyinmajmecvwlsbhpidepxjhumzsqnlomywmecouzeivqgyklmbcsocasmuujunlcrlhfpndmrachslawrakmbuvvminxernkhblzrqifwqiywotgnuvbqkowhbaxqoklvydaidaemqrowhtjdgoaggddiqfnmxcnqtzyvlpwwjespbainutsjkjsrobsbiygmvjddrczokdywzknmsknujfslzmwerkgdsuqcrjkxopomwrxpnumamrhjkskhcsmsfopgiuipdvyhebffchpvlbjaopmpgrtznkmdvyigppfyjcquuypljmausylxmbrsnqlfzhaicfpgeevyjhbxlhrzpdkqjewywmnrwgysvjcxqfgyixykecavgfgzzhtwtpnpfzmcpgxtscttlzfthfqwfffemiylmbduxlqzbwqfkzmnppcgfbcqdsfdvnlfxgauvoynxjzzcvjkcqrvehicwqkbqnhhekcrvqxioujrtgkugffuoaqunboejjmlsepbzlrgcnfifpqcxpnbmetgyuicocbekkukdpxtjtvtndrdworrcktummfhixuscwzslvvfkfsuhnqlntgexhcgbcqybpeqypgavcfwdcrsotykrjuwrrokkfuflgzopvvawwstjiwtommxlgpnxnvnszuqlqqenamapbqqcystdgwqknvtkmqqkmvwfmfwdpfazwpejmcwbstruibmaulxmeuzpepunzswazjbgzegtqndpouceslphccgmblgmggmxenkemjzakmetnntmjkgwwtjjomxvejxnnecokvegvybijmlosslaouamnabgdmuwtsckjdfwkrooemmvbdpkgahoykthrbfttntqfeybhjxtnpyeqwxmthtulrjrolveaksymrcohrxbmdmyvzichkksvzeqsztbrskvpntwxxpjvfrhyopvsoqzgokhqemrssytxizqzjsnbdfwyjzfsgnimptajueyixxhcekeskehyihwntlukzpindbxduffmqikweqzcxdtomsqlnnnrsdgtfwiuhymvjhljdudwxlnlxfyvfqwxgppbefixfqetuciniyokrtetajntedelzzwkcknryxjkehmhkfzoavmxlxtwcdbuoorridsorlmfrxentjrladhgexulcxwpvxgrymvenjjjuzgxkgbfcxrwcafhflaricqtwljevzjsooyrgkrluulbyrpnoecjyrsgppxpsmqhtrdlmyexcmzeabycgdttndziitnsqflsfbbogwseehuhqebgtfzhsuzkfiiygmppexymmdjbqihlpshvueqqwltsmllpuryqufmfzhpcqowtoempnmdmezcnxfwzykrrmmuhhkrybjqldfsbyfhxfzknyjnhjqskssvladktiklvevpwkhveuzdfpqqcvbbknweqagugxbbkigiooopnflqlqwpadpszzjtyjmlqxqnpwuaskkoavwgpaztchajuufdvtyvncqktevhtrtblwfhowgubdugyiryuhyskqmeeavuxtypqmxdbhypxxzjrsaameqyfhbgmfvbhgzymymukviqwwontdzyrfqmnvmtpcrurnrnjbniyprekqnzhqsbpphkiogfafxakfyafibkiupvpliupyhxbpmhkhrmhnyogbdyxhitaozyxzoxggsouwwhhnhcxrqfgyhhjytapmgyplxgjtokwnlqsykmrazzliiaesljutcpjxxkinupdtkfmkdusitibatwfsjkabfmpgkcsqbjjrwvlylrinvgsniqvdfgpwrrlmyrmonzmpgwqmaomrfetgraxtlgicsrmfbfijvgfannqnvlrcwkkmjppwuopcgvrechvlgxlurzcprqhzldptmvggxtemtinssfordlxtbthpjarvuylrhdaabzofnjavrztznlcftrwgpxnbtgidkupccaipjegphcdbjwwqsgnlxgrdemebtmucnpdwqmifyonkigsrlyjllcalbmmgyvtjqinvuciulavugtwbqhafjhzwufyrsbgaplmxsctkqtxfsgcefirkkegjuwaflzudlnrlwxsuyodmucqwkisrytheurkjqkvurotiomhckbbwduwgsnjyazdctchrhaxtcdzunwohuklligzsstqizhbwtyzddkwnzmtskdyxmglrysjiqtjkukupflyeiimswqvisjtvsucrchuwqfumupiqigcunzdfxrruferfwgyvmpljjbtddmvldvszqiqvadfawvqdjdmiezsfomohsmdjtysajkkfisfriqhvopmfkegrxqxaylgcemovnmoafpkvyyediuhhjsgipbsuppfldleeskhdzmmrhvpxyvomruqddkoelfrrsptuifnnvblnmphxngopaxjzbgzwoggqjncldoiqkmvgcalpygcqsolosmdzqlsgsgmimumsrmgpmmbatxtalvypkfukhvhhwrcsktqepaceyzxxrpwkloiihlahlnuvjzwhvoqgenammjjjectioxiyrscvrfmnmletsmebqvloqyknqzfscmrjflflomwvrrlrnqifgydkrdtjfqraiihtlmxsqkgybymxfzuqlfzbtlawrtvekhngcedcbsyqnqynmcwqakdgnmxelfccdvdhpanamqoaljjuqpbmwyoxwztoagwtpuvsjkiangpprilaqzrttkavyevnuuvrgqkwortevosugdlkkdhqykdmgmuvvxnpaeifhrlstomqbhmjpoaziotceiarlyrjhx)";
            co_return huge_string;
        });

    server.register_method_ct<urpc::method_id("Example.Sleep2s")>(
        [](urpc::RpcContext &ctx,
           std::span<const uint8_t> body)
    -> task::Awaitable<std::vector<uint8_t> > {
            using namespace std::literals::chrono_literals;
            ulog::info("SERVER: Example.Sleep2s called, body_size={}", body.size());

            co_await system::this_coroutine::sleep_for(2000ms);

            static constexpr char msg[] = "slept 2 seconds";
            std::vector<uint8_t> out(msg, msg + sizeof(msg) - 1);
            co_return out;
        });

    server.register_method_ct<urpc::method_id("System.Shutdown")>(
        [&uvent](urpc::RpcContext &,
                 std::span<const uint8_t>)
    -> task::Awaitable<std::vector<uint8_t> > {
            ulog::warn("SERVER: System.Shutdown invoked");

            const bool was_first =
                    !shutdown_initiated.exchange(true, std::memory_order_acq_rel);

            if (was_first) {
                ulog::warn("SERVER: cancel summary: "
                           "before={} after={} bytes_dropped={}",
                           cancel_before_handler.load(std::memory_order_relaxed),
                           cancel_after_handler.load(std::memory_order_relaxed),
                           cancel_bytes_dropped.load(std::memory_order_relaxed));

                std::thread([&uvent]() {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(500));
                    ulog::warn("SERVER: calling uvent.stop()");
                    uvent.stop();
                }).detach();
            } else {
                ulog::warn("SERVER: System.Shutdown already in progress");
            }

            static constexpr char reply[] = "bye";
            std::vector<uint8_t> out(reply, reply + sizeof(reply) - 1);
            co_return out;
        });

    ulog::info("SERVER: System.Shutdown handler registered");

    uvent.for_each_thread([&](int threadIndex, thread::ThreadLocalStorage *) {
        system::co_spawn_static(server.run_async(), threadIndex);
    });
    ulog::info("SERVER: accept_loop spawned on {} threads, calling uvent.run()",
               kThreads);

    uvent.run();

    ulog::warn("SERVER: uvent.run() returned");
    ulog::warn(
        "SERVER: final cancel summary: before={} after={} bytes_dropped={}",
        cancel_before_handler.load(std::memory_order_relaxed),
        cancel_after_handler.load(std::memory_order_relaxed),
        cancel_bytes_dropped.load(std::memory_order_relaxed));

    usub::ulog::shutdown();

    return 0;
}
