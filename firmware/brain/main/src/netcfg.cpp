/**
 * netcfg.cpp - W5500 Ethernet bring-up + persisted LAN config + SCPI TCP server.
 * See netcfg.h for the pin map and the design overview.
 */
#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EEPROM.h>
#include <string.h>
#include <pico/unique_id.h>

#include "netcfg.h"
#include "scpi.h"
#include "channel_link.h"   // keep the channel watchdogs fed across a DHCP attempt

// ── W5500 pin map (RP2350 hardware SPI0) ────────────────────────────────────────
#define NET_PIN_CS   17
#define NET_PIN_SCK  18
#define NET_PIN_MOSI 19
#define NET_PIN_MISO 20
#define NET_PIN_RST  21
// GPIO22 (INT) is intentionally unused: the Arduino Ethernet library polls the
// W5500 over SPI rather than taking an interrupt.

// ── SCPI transport limits ───────────────────────────────────────────────────────
#define NET_MAX_CLIENTS 4      // concurrent SCPI sockets (W5500 has 8 hw sockets)
#define NET_LINE_MAX    256    // SCPI line length cap (spec §2)

// EthernetClient::stop() politely sends a FIN and then spins on socketStatus()
// until the socket reaches CLOSED or this timeout expires, before forcing the
// close. The library default is 1000 ms — and a peer that vanished (host
// crashed, cable pulled, script killed) never completes the handshake, so every
// such socket costs the full budget. netcfg_task() can call stop() once per
// pool slot per pass, so the default turns a few half-open sockets into a
// multi-second stall of loop(): the front panel stops responding entirely and
// the channel comms watchdogs (1000 ms) flap. We do not need a graceful close
// here — the SCPI session carries no state worth draining — so keep the budget
// short and let the forced socketClose() do the work.
#define NET_CLIENT_CLOSE_MS 20u

// ── Reclaiming abandoned sessions ───────────────────────────────────────────────
// A slot is otherwise freed only when connected() goes false, i.e. when the peer
// closes properly. A peer that vanishes without a FIN (cable pulled, host slept,
// machine powered off) leaves its socket ESTABLISHED forever, so its slot is
// never reclaimed; four of those and the instrument is unreachable over LAN with
// nothing whatsoever wrong with loop(). Confirmed on the bench - see
// firmware/brain/NETWORK_HANG_HANDOVER.md §0b. Two independent backstops:
//
// 1. The W5500's own TCP keep-alive (Sn_KPALVTR), off by default. With it set,
//    the chip probes an idle peer by itself and drops the socket when the probes
//    go unanswered, so connected() goes false and the normal reaping applies.
//    Units of 5 s; 12 = 60 s, so a vanished peer costs at most ~2 min.
// 2. A last-activity timestamp per slot, for the case keep-alive cannot catch:
//    the peer's TCP stack is alive and answers probes, but the application
//    behind it is gone and will never send another command. Generous, because
//    it also applies to a perfectly healthy session that is simply idle between
//    measurements - dropping one of those is harmless (the host reconnects) but
//    should not be routine.
#define NET_CLIENT_KEEPALIVE_5S 12u      /* 12 x 5 s = 60 s */
#define NET_CLIENT_IDLE_MS      600000u  /* 10 min */

// Bytes of SCPI input consumed per netcfg_task() pass, shared across all clients.
// Each complete line is dispatched to the parser inline, and a MEASure can cost
// ~50 ms per channel (scpi.cpp measForce), so an unbounded drain would let one
// client pipelining commands stall loop() for seconds - and trip the watchdog.
// Whatever is left stays in the W5500's RX buffer until the next pass. Normal
// interactive use sends one line per pass and never reaches this.
#define NET_RX_BUDGET_PER_PASS 128

// ── DHCP timing ─────────────────────────────────────────────────────────────────
// Every DHCP acquisition in this library is *blocking* (Dhcp.cpp spins until it
// has a lease or the timeout expires), so every millisecond spent here is a
// millisecond loop() is not polling the front panel or feeding the channels.
//
// The initial lease attempt at boot is therefore skipped entirely unless the PHY
// already reports link-up: with no cable there is nobody to answer, and spending
// the full timeout would leave the front panel dead for seconds after power-on for
// no gain. It is also bounded, so a cable into a network without a DHCP server can
// only stall startup briefly (the OLED is already showing the readout by then, and
// the channel outputs are still off). If it is skipped or fails, netcfg_task()
// retries while the cable is up — promptly on the link-up edge, then on a slow
// cadence — so the supply self-heals once the network appears without any panel
// interaction.
//
// The budget is deliberately shorter than the channel-side comms watchdog
// (COMMS_TIMEOUT_MS = 1000 ms): an attempt that outran it would make both channels
// drop their outputs every retry period. A LAN DHCP server answers in tens of
// milliseconds, so 800 ms is ample; if none does, we simply try again later.
//
// One budget for *every* DHCP transaction, deliberately. Ethernet.begin() is the
// only way to arm DhcpClass::_timeout, and that same value then bounds the lease
// renewals that maintain() performs later. A generous boot budget therefore is
// not free: it is silently inherited by every future renewal, where it freezes
// loop() long after boot. Since the retry path below re-attempts every 20 s with
// the front panel alive in between, a short uniform budget costs nothing at boot
// (worst case: the address appears 20 s later) and bounds renewals for good.
#define NET_DHCP_INIT_TIMEOUT_MS  800
#define NET_DHCP_INIT_RESP_MS     250
#define NET_DHCP_RETRY_TIMEOUT_MS 800
#define NET_DHCP_RETRY_RESP_MS    250
#define NET_DHCP_RETRY_PERIOD_MS  20000u
// Settle time granted to the switch port after a link-up edge before the first
// retry fires (autoneg/STB learning would swallow a DISCOVER sent immediately).
#define NET_DHCP_LINKUP_DELAY_MS  1000u

// ── Persisted configuration ─────────────────────────────────────────────────────
// Own EEPROM slot (magic/version/CRC), separate from the thermal/runtime/setpoint
// stores in main.cpp. Base address 96 leaves room below for those blobs.
#define NET_STORE_ADDR    96
#define NET_STORE_MAGIC   0x4E455431u   /* 'NET1' */
#define NET_STORE_VERSION 1

struct NetStore {
    uint32_t magic;
    uint16_t version;
    uint8_t  dhcp;                        // 1 = DHCP, 0 = static
    uint8_t  reserved;
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    char     hostname[NETCFG_HOSTNAME_MAX];
    uint16_t crc;
};

// Live (pending) config, seeded with defaults; overwritten by cfgLoad().
static bool    s_dhcp        = true;
static uint8_t s_ip[4]       = { 192, 168, 1, 50 };
static uint8_t s_mask[4]     = { 255, 255, 255, 0 };
static uint8_t s_gw[4]       = { 192, 168, 1, 1 };
static uint8_t s_dns[4]      = { 192, 168, 1, 1 };
static char    s_hostname[NETCFG_HOSTNAME_MAX] = "psu-2ch";

static uint8_t s_mac[6];               // derived from the chip id at init
static bool    s_hwPresent   = false;
static bool    s_inited      = false;
static bool    s_applyPending = false;
static uint32_t s_lastRetryMs = 0;
static bool    s_prevLink    = false;   // link-up edge detector for the retry clock
// True only while we believe we hold a DHCP lease. This gates Ethernet.maintain()
// — see netcfg_task() for why calling it without a lease is fatal to the UI.
static bool    s_leased      = false;

// ── SCPI TCP server + client pool ───────────────────────────────────────────────
static EthernetServer s_server(NETCFG_SCPI_PORT);
static EthernetClient s_clients[NET_MAX_CLIENTS];
static char           s_line[NET_MAX_CLIENTS][NET_LINE_MAX];
static uint16_t       s_lineLen[NET_MAX_CLIENTS];
static bool           s_lineOverflow[NET_MAX_CLIENTS];
static uint32_t       s_lastActiveMs[NET_MAX_CLIENTS];   // for NET_CLIENT_IDLE_MS

// CRC16-CCITT (poly 0x1021, init 0xFFFF) — same routine as the main.cpp stores.
static uint16_t netCrc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void deriveMac() {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    // Locally-administered, unicast MAC from the low chip-id bytes.
    memcpy(s_mac, &id.id[2], 6);
    s_mac[0] &= (uint8_t)~0x01;   // unicast
    s_mac[0] |= 0x02;             // locally administered
}

static void cfgLoad() {
    NetStore s;
    EEPROM.get(NET_STORE_ADDR, s);
    if (s.magic != NET_STORE_MAGIC || s.version != NET_STORE_VERSION) return;
    if (s.crc != netCrc16((const uint8_t *)&s, offsetof(NetStore, crc)))  return;
    s_dhcp = s.dhcp != 0;
    memcpy(s_ip,   s.ip,   4);
    memcpy(s_mask, s.mask, 4);
    memcpy(s_gw,   s.gw,   4);
    memcpy(s_dns,  s.dns,  4);
    s.hostname[NETCFG_HOSTNAME_MAX - 1] = '\0';
    strncpy(s_hostname, s.hostname, NETCFG_HOSTNAME_MAX);
    s_hostname[NETCFG_HOSTNAME_MAX - 1] = '\0';
}

void netcfg_save() {
    NetStore s;
    memset(&s, 0, sizeof(s));
    s.magic   = NET_STORE_MAGIC;
    s.version = NET_STORE_VERSION;
    s.dhcp    = s_dhcp ? 1 : 0;
    memcpy(s.ip,   s_ip,   4);
    memcpy(s.mask, s_mask, 4);
    memcpy(s.gw,   s_gw,   4);
    memcpy(s.dns,  s_dns,  4);
    strncpy(s.hostname, s_hostname, NETCFG_HOSTNAME_MAX);
    s.hostname[NETCFG_HOSTNAME_MAX - 1] = '\0';
    s.crc = netCrc16((const uint8_t *)&s, offsetof(NetStore, crc));
    EEPROM.put(NET_STORE_ADDR, s);
    EEPROM.commit();   // RP2350 EEPROM emulation is a RAM shadow; commit flushes it
}

// Hardware-reset the W5500 and re-configure the interface from the pending config.
static void bringUp() {
    // Hardware reset pulse (W5500 needs >=500 us low, then ~50 ms to settle).
    pinMode(NET_PIN_RST, OUTPUT);
    digitalWrite(NET_PIN_RST, LOW);
    delay(2);
    digitalWrite(NET_PIN_RST, HIGH);
    delay(60);

    // Route SPI0 to the W5500 pins before the library opens the bus.
    SPI.setRX(NET_PIN_MISO);
    SPI.setCS(NET_PIN_CS);
    SPI.setSCK(NET_PIN_SCK);
    SPI.setTX(NET_PIN_MOSI);
    SPI.begin();

    Ethernet.init(NET_PIN_CS);

    if (s_dhcp) {
        // linkStatus() probes/initialises the chip itself, so it is safe (and cheap)
        // before any begin(). With no cable there is no point paying the DHCP
        // timeout: configure the interface unaddressed and leave acquisition to the
        // link-gated retry in netcfg_task().
        if (Ethernet.linkStatus() == LinkON) {
            // Bounded, so a network without a DHCP server can't hang boot for long.
            s_leased = (Ethernet.begin(s_mac, NET_DHCP_INIT_TIMEOUT_MS,
                                       NET_DHCP_INIT_RESP_MS) == 1);
        } else {
            s_leased = false;
            IPAddress zero(0, 0, 0, 0);
            Ethernet.begin(s_mac, zero, zero, zero, zero);   // non-blocking
        }
    } else {
        s_leased = false;                           // static: nothing to renew
        IPAddress ip(s_ip[0], s_ip[1], s_ip[2], s_ip[3]);
        IPAddress dns(s_dns[0], s_dns[1], s_dns[2], s_dns[3]);
        IPAddress gw(s_gw[0], s_gw[1], s_gw[2], s_gw[3]);
        IPAddress mask(s_mask[0], s_mask[1], s_mask[2], s_mask[3]);
        Ethernet.begin(s_mac, ip, dns, gw, mask);   // non-blocking
    }

    s_hwPresent = (Ethernet.hardwareStatus() != EthernetNoHardware);
    s_lastRetryMs = millis();
    s_server.begin();
}

void netcfg_init() {
    deriveMac();
    cfgLoad();

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        s_lineLen[i]      = 0;
        s_lineOverflow[i] = false;
        s_lastActiveMs[i] = 0;
    }

    bringUp();
    s_inited = true;
}

void netcfg_apply() {
    netcfg_save();
    if (!s_inited) return;
    // Drop any live SCPI sessions; the interface is about to be reconfigured.
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (s_clients[i]) s_clients[i].stop();
        s_lineLen[i]      = 0;
        s_lineOverflow[i] = false;
        s_lastActiveMs[i] = 0;
    }
    bringUp();
}

void netcfg_request_apply() { s_applyPending = true; }

// Response sink handed to the SCPI parser: write straight to the originating
// TCP client. `ctx` is the EthernetClient* for that session.
static void netWrite(void *ctx, const char *data, size_t len) {
    EthernetClient *c = (EthernetClient *)ctx;
    if (c && c->connected() && len > 0)
        c->write((const uint8_t *)data, len);
}

// Feed received bytes for one client through its line assembler, dispatching a
// complete line (LF-terminated, CR ignored) to the SCPI parser. Over-long lines
// are dropped and flagged so the terminating LF reports a single framing error.
// Consumes at most `budget` bytes and returns how many it took, so netcfg_task()
// can share one bounded budget across the whole pool.
//
// Bytes are pulled in blocks rather than one at a time: EthernetClient::read()
// costs a full SPI transaction per call, so the byte-at-a-time form spent one
// transaction (and, every 250 bytes, a Sock_RECV command) per character.
static int serviceClient(int slot, int budget) {
    EthernetClient &c = s_clients[slot];
    int avail = c.available();
    if (avail <= 0 || budget <= 0) return 0;
    if (avail > budget) avail = budget;

    s_lastActiveMs[slot] = millis();

    uint8_t chunk[64];
    int consumed = 0;
    while (consumed < avail) {
        int want = avail - consumed;
        if (want > (int)sizeof(chunk)) want = (int)sizeof(chunk);
        int got = c.read(chunk, (size_t)want);
        if (got <= 0) break;
        consumed += got;
        for (int i = 0; i < got; i++) {
            char ch = (char)chunk[i];
            if (ch == '\n') {
                if (s_lineOverflow[slot]) {
                    scpi_report_line_overflow(netWrite, &c);
                    s_lineOverflow[slot] = false;
                } else {
                    s_line[slot][s_lineLen[slot]] = '\0';
                    scpi_process_line(s_line[slot], netWrite, &c);
                }
                s_lineLen[slot] = 0;
                // A failed response means socketSend() hit its budget: the peer
                // has stopped reading (shut window) or gone away. Every further
                // reply queued behind it would pay that budget again, so drop
                // the session now rather than spending several hundred ms per
                // pass writing into a hole. A client that will not read its
                // answers has broken the request/response contract anyway.
                if (c.getWriteError()) {
                    c.clearWriteError();
                    c.setConnectionTimeout(NET_CLIENT_CLOSE_MS);
                    c.stop();
                    s_lineLen[slot]      = 0;
                    s_lineOverflow[slot] = false;
                    return consumed;
                }
            } else if (ch != '\r') {
                if (s_lineLen[slot] < NET_LINE_MAX - 1) {
                    s_line[slot][s_lineLen[slot]++] = ch;
                } else {
                    s_lineOverflow[slot] = true;   // keep consuming until the LF
                }
            }
        }
    }
    return consumed;
}

void netcfg_task() {
    if (!s_inited) return;

    // Deferred reconfigure requested by a SCPI LAN setter (done here, outside the
    // per-client RX loop, so we never tear down a socket mid-read).
    if (s_applyPending) { s_applyPending = false; netcfg_apply(); return; }

    // Accept a newly-connected client into a free (or dead) slot; refuse if full.
    EthernetClient incoming = s_server.accept();
    if (incoming) {
        int slot = -1;
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (!s_clients[i] || !s_clients[i].connected()) { slot = i; break; }
        }
        incoming.setConnectionTimeout(NET_CLIENT_CLOSE_MS);
        if (slot < 0) {
            incoming.stop();                 // pool full: reject
        } else {
            if (s_clients[slot]) s_clients[slot].stop();
            s_clients[slot]      = incoming;
            s_lineLen[slot]      = 0;
            s_lineOverflow[slot] = false;
            s_lastActiveMs[slot] = millis();
            // Let the chip police this peer for us, so a host that disappears
            // without closing cannot hold the slot indefinitely.
            s_clients[slot].setKeepAlive(NET_CLIENT_KEEPALIVE_5S);
        }
    }

    // Drain each live client; reap the ones that dropped or went silent. The
    // close budget is re-applied here because EthernetClient's assignment
    // operator copies the socket index but each object carries its own _timeout.
    // The RX budget is shared across the pool so the total SCPI work dispatched
    // per pass stays bounded no matter how many clients are pipelining.
    int rxBudget = NET_RX_BUDGET_PER_PASS;
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (!s_clients[i]) continue;
        if (!s_clients[i].connected()) {
            s_clients[i].setConnectionTimeout(NET_CLIENT_CLOSE_MS);
            s_clients[i].stop();
            continue;
        }
        if ((uint32_t)(millis() - s_lastActiveMs[i]) >= NET_CLIENT_IDLE_MS) {
            s_clients[i].setConnectionTimeout(NET_CLIENT_CLOSE_MS);
            s_clients[i].stop();             // idle too long: reclaim the slot
            s_lineLen[i]      = 0;
            s_lineOverflow[i] = false;
            continue;
        }
        rxBudget -= serviceClient(i, rxBudget);
    }

    // DHCP lease renewal. Only ever call maintain() while we actually hold a lease:
    // with none, the library's checkLease() matches its "_rebindInSec == 0 &&
    // _dhcp_state == STATE_DHCP_START" arm — precisely the state a *failed* attempt
    // leaves behind — and re-runs the blocking request_DHCP_lease() on every single
    // call, at the timeout of the last begin(). Unguarded, a booted-with-no-DHCP-
    // server unit therefore stalls loop() every pass, forever: the front panel
    // stops responding and both channel links flap on their 500 ms telemetry
    // timeout. Re-acquisition belongs to the bounded, rate-limited retry below.
    //
    // maintain() is *also* blocking when a renewal actually comes due:
    // DhcpClass::checkLease() calls request_DHCP_lease() for the renew, and can
    // fall straight into the rebind arm and call it a second time in the same
    // pass — each bounded only by the _timeout the last begin() armed. That is
    // why NET_DHCP_INIT_TIMEOUT_MS is now as short as the retry budget: a
    // generous boot value used to be inherited here, so a single renewal could
    // freeze loop() for twice that, minutes into a session, with the front panel
    // dead and both channel links flapping. The channels are pinged either side
    // exactly as the re-acquisition path does.
    if (s_dhcp && s_leased) {
        channel_ping(0); channel_ping(1);
        int rc = Ethernet.maintain();
        channel_ping(0); channel_ping(1);
        channel_link_task();
        // 1 = renew failed, 3 = rebind failed: the lease is gone and the library is
        // back in STATE_DHCP_START, so stop calling maintain() and let the retry
        // path (with its short budget) do the re-acquisition.
        if (rc == 1 || rc == 3) s_leased = false;
    }

    // Slow DHCP retry: reacquire a lease if we booted before the network came up,
    // or lost it later. Gated on link-up, so an unplugged cable costs nothing.
    bool linkUp = netcfg_link_up();
    if (linkUp && !s_prevLink) {
        // Cable just appeared: don't make the user wait out a whole retry period.
        // Backdate the retry clock so the next attempt fires after the settle time.
        s_lastRetryMs = millis() - (NET_DHCP_RETRY_PERIOD_MS - NET_DHCP_LINKUP_DELAY_MS);
    }
    s_prevLink = linkUp;

    if (s_dhcp && !s_leased && linkUp) {
        uint32_t now = millis();
        if ((uint32_t)(now - s_lastRetryMs) >= NET_DHCP_RETRY_PERIOD_MS) {
            s_lastRetryMs = now;
            // The attempt blocks for up to NET_DHCP_RETRY_TIMEOUT_MS with the UART
            // links unserviced. Ping both channels first so their comms watchdogs
            // start the window fresh, and again after so a link marked down on the
            // brain side recovers on the next telemetry push instead of waiting.
            channel_ping(0); channel_ping(1);
            s_leased = (Ethernet.begin(s_mac, NET_DHCP_RETRY_TIMEOUT_MS,
                                       NET_DHCP_RETRY_RESP_MS) == 1);
            channel_ping(0); channel_ping(1);
            channel_link_task();
        }
    }
}

// ── Pending-config accessors ────────────────────────────────────────────────────
bool netcfg_dhcp()              { return s_dhcp; }
void netcfg_set_dhcp(bool on)   { s_dhcp = on; }

void netcfg_get_ip(uint8_t o[4])    { memcpy(o, s_ip, 4); }
void netcfg_set_ip(const uint8_t v[4])   { memcpy(s_ip, v, 4); }
void netcfg_get_mask(uint8_t o[4])  { memcpy(o, s_mask, 4); }
void netcfg_set_mask(const uint8_t v[4]) { memcpy(s_mask, v, 4); }
void netcfg_get_gw(uint8_t o[4])    { memcpy(o, s_gw, 4); }
void netcfg_set_gw(const uint8_t v[4])   { memcpy(s_gw, v, 4); }
void netcfg_get_dns(uint8_t o[4])   { memcpy(o, s_dns, 4); }
void netcfg_set_dns(const uint8_t v[4])  { memcpy(s_dns, v, 4); }

const char *netcfg_hostname()   { return s_hostname; }
void netcfg_set_hostname(const char *s) {
    if (!s) return;
    strncpy(s_hostname, s, NETCFG_HOSTNAME_MAX);
    s_hostname[NETCFG_HOSTNAME_MAX - 1] = '\0';
}

// ── Live status ─────────────────────────────────────────────────────────────────
bool netcfg_hw_present() { return s_hwPresent; }
bool netcfg_link_up()    { return s_inited && Ethernet.linkStatus() == LinkON; }

bool netcfg_has_ip() {
    IPAddress ip = Ethernet.localIP();
    return !(ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0);
}

static void ipToBytes(IPAddress ip, uint8_t o[4]) {
    o[0] = ip[0]; o[1] = ip[1]; o[2] = ip[2]; o[3] = ip[3];
}
void netcfg_live_ip(uint8_t o[4])   { ipToBytes(Ethernet.localIP(),   o); }
void netcfg_live_mask(uint8_t o[4]) { ipToBytes(Ethernet.subnetMask(),o); }
void netcfg_live_gw(uint8_t o[4])   { ipToBytes(Ethernet.gatewayIP(), o); }
void netcfg_mac(uint8_t o[6])       { memcpy(o, s_mac, 6); }

int netcfg_client_count() {
    int n = 0;
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        if (s_clients[i] && s_clients[i].connected()) n++;
    return n;
}
