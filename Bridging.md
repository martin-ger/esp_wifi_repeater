# L2 Bridging on the ESP8266 (`VARIANT=bridge`)

This document describes how the transparent layer-2 bridge between the
upstream WiFi (STA interface) and the local soft-AP works. All bridge logic
lives in `user/bridge.c`, compiled in only when `REPEATER_MODE` is defined
(set by `user/user_config_bridge.h`). The default `make` build still uses
NAPT; `make VARIANT=bridge` selects the bridge.

The goal of the bridge is to make AP-side clients appear on the same IP
subnet as the upstream router. They get IPs from the upstream DHCP server,
ARP normally, and are reachable both from upstream and from each other –
all while a single ESP8266 radio is doing the work.

## Why this is non-trivial

A real Ethernet bridge moves frames between ports without rewriting them.
A WiFi "bridge" cannot do that, because of an 802.11 hard rule:

> A frame transmitted by an AP on its BSS must carry the AP's own BSSID/MAC
> as the source. A frame transmitted by a STA must carry the STA's MAC as
> the source.

So we cannot just forward an AP-side client's frame upstream verbatim – the
upstream AP would drop it. The bridge therefore rewrites L2 source MACs at
both interfaces and uses two helpers to keep L3 reachability working
correctly:

* **FDB** – a small forwarding database mapping `client IP → real client MAC`.
  Populated from observed traffic and DHCP. Used to translate the upstream's
  unicast frames (addressed to the AP MAC) back to the right client on the
  AP side.
* **Proxy ARP** – on each interface, the bridge answers ARP requests on
  behalf of the other side using the local interface's own MAC, so neither
  side ever sees the foreign MAC of a peer it cannot reach directly.

A custom mini mDNS responder (~100 lines) is also included because the SDK's
built-in `espconn_mdns_init` cannot be made to listen on the AP netif.

## Files and entry points

| Item | Where |
|---|---|
| Bridge implementation | `user/bridge.c` |
| Public API | `user/bridge.h` (`bridge_init`, `bridge_show_fdb`) |
| Activation flag | `REPEATER_MODE` in `user/user_config_bridge.h` |
| Bridge bring-up | `user_main.c` – `bridge_init(sta_nif, ap_nif)` is called on STA connect |

`bridge_init` saves the original `input`, `output`, and `linkoutput` function
pointers of both netifs, then replaces `input` and `output` with the bridge's
hook functions. `linkoutput` is left untouched – the bridge calls the saved
originals directly when it needs to inject a frame on the wire. The STA
netif is also made the default route, NAPT is disabled on both netifs, and
the soft-AP channel is forced to match the STA channel.

## Datapath overview

There are four hooks. Two are inputs (frames coming up from a radio), two
are outputs (IP packets going down from the local lwip stack):

```
       upstream WiFi (STA)                    AP-side client
              │  ▲                                 ▲  │
              │  │                                 │  │
              ▼  │ s_orig_lo_sta            s_orig_lo_ap ▲
   bridge_input_sta ◄──── frame from STA radio          │
   bridge_output_sta ───► IP packet, default route      │
                                 │                      │
                                 │                      │
                                 ▼                      │
                            local lwip stack             │
                                 ▲                      │
                                 │                      │
                                 ▼                      │
   bridge_output_ap   ───► IP packet via AP netif       │
   bridge_input_ap    ◄──── frame from AP radio ────────┘
```

The two `input` hooks always work on a copied pbuf `q`. They classify the
frame, possibly forward a copy, possibly hand the original up to the local
stack, and free `q` before returning.

## The FDB (forwarding database)

```c
#define FDB_SIZE   16
#define FDB_TTL_S 600
typedef struct { uint32_t ip; uint8_t mac[6]; uint32_t expires_s; } fdb_entry_t;
```

Sixteen entries, ten-minute TTL, replace the oldest on overflow. Lookups are
linear; that is fine for a handful of clients on an ESP8266.

`fdb_insert` rejects three special cases:

```c
if (ip == 0) return;
if (s_sta_nif && ip == s_sta_nif->ip_addr.addr) return;
if (s_ap_nif  && ip == s_ap_nif->ip_addr.addr)  return;
```

The own-IP guards are important: without them an AP-side client that
generates traffic with an unfortunate source IP, or a DHCP collision against
the STA address, can poison the FDB and make the management IP look like
it lives on a client MAC. After that, ICMP/TCP to the management IP gets
forwarded to the wrong client instead of being delivered locally.

The bridge populates the FDB from three sources:

* Every IPv4 packet arriving on the AP side (`ip->src` → original `eth->src`),
  done **before** any early return.
* Every ARP packet arriving on the AP side (`arp->spa` → original `eth->src`).
* DHCP ACK snooping on the STA side (`yiaddr` → recovered original chaddr,
  with the DHCP lease time as the TTL).

It is consulted by `bridge_input_sta` to find the right destination MAC for
unicast frames coming down from upstream, and by both output hooks to route
IP packets generated by the local stack to the correct side.

## Normal IP traffic

### Downstream (AP client → upstream)

`bridge_input_ap` runs:

1. **FDB learning first.** The source IP/MAC pair is inserted into the FDB
   before any early-return check. This is critical for the management-IP
   reachability case (see ICMP example below).
2. **Frames addressed to the AP MAC** that aren't broadcast/multicast are
   handed straight to `s_orig_input_ap` so the local lwip stack can process
   them. These are packets like ICMP echo to the management IP, or TCP to the
   web UI / console.
3. **Anything else** is rewritten: `eth->src` is replaced with the STA MAC
   and the frame is sent upstream via `s_orig_lo_sta`. From the upstream
   AP's perspective, it sees a single STA (us) sending traffic – which
   satisfies the 802.11 rule.
4. **Broadcast/multicast frames** are *also* passed to `s_orig_input_ap`
   after forwarding, so the local stack can react (e.g. DHCP, mDNS).

### Upstream (upstream → AP client)

`bridge_input_sta` runs:

1. Frames whose `eth->src` is the AP's own MAC are returned to the local
   stack untouched – we have no business looping our own transmits back.
2. For unicast IP frames addressed to the STA MAC (us) but whose IP `dst`
   belongs to a known AP-side client (`fdb_lookup(ip->dst)` succeeds), the
   destination MAC is rewritten to that client's MAC, the source MAC to the
   AP MAC, and the frame is sent down via `s_orig_lo_ap`. The original `p`
   is dropped (return `ERR_OK` after `pbuf_free(p)`).
3. Frames truly addressed to the STA MAC (e.g. responses to local stack's
   own connections) fall through to `s_orig_input_sta`.
4. Broadcast and multicast IP frames are sent down to the AP side and also
   passed to the local stack.

### Locally-generated IP (ICMP reply, web UI, mDNS responder, etc.)

When the local lwip stack itself generates a packet, it goes through one of
the output hooks depending on which netif lwip routed it through.

`bridge_output_sta(netif, p, ipaddr)`:

* If `fdb_lookup(ipaddr->addr)` returns a MAC, the destination is an AP-side
  client. We prepend an Ethernet header (dst = client MAC, src = AP MAC) and
  inject the frame via `s_orig_lo_ap`, bypassing lwip's ARP entirely – we
  already know the MAC.
* If the destination is IPv4 multicast (224/4), we additionally craft the
  standard `01:00:5e:xx:xx:xx` MAC and copy the frame to the AP side via
  `s_orig_lo_ap` *before* letting `s_orig_output_sta` send it upstream.
* Otherwise, hand off to `s_orig_output_sta` – upstream traffic from the
  local stack.

`bridge_output_ap(netif, p, ipaddr)` mirrors this for the AP netif. Without
this hook, lwip would try to ARP-resolve the client IP on the AP netif – but
the AP netif's IP/subnet (`172.31.255.1/24`) bears no relation to the
upstream subnet, so ARP would fail and lwip would emit a useless gratuitous
ARP for its own dummy address. Using the FDB instead, we go straight from
"I have an IP" to "I have a MAC and a link to send on".

### Concrete example: pinging the management IP from an AP client

1. AP client sends `ARP who-has 192.168.178.104?` (the STA IP). This is
   broadcast.
2. `bridge_input_ap` sees `is_bcast = true`. Early FDB-learn block runs and
   `client_IP → client_MAC` is inserted.
3. The ARP block detects `arp->tpa == sta_nif->ip_addr.addr` and replies
   with `send_proxy_arp_reply` carrying the AP MAC. The client's ARP cache
   now has `192.168.178.104 → AP-MAC`.
4. AP client sends `ICMP echo` with `eth->dst = AP-MAC`. `is_to_ap_mac` is
   true, so the bridge passes the frame straight to `s_orig_input_ap`. The
   FDB-learn block has already recorded the client.
5. lwip generates the echo reply: `src = STA-IP`, `dst = client-IP`. Routing
   sends it via the default netif (STA), so `bridge_output_sta` is invoked.
6. `fdb_lookup(client-IP)` finds the MAC (step 2's learn). The bridge
   prepends the Ethernet header and calls `s_orig_lo_ap`. Reply arrives at
   the client.

The first version of the bridge did the FDB insert *after* the
`is_to_ap_mac` early return, which broke this case – the ICMP reply went
out the STA interface to the upstream router instead.

## DHCP

DHCP is the trickiest protocol on the bridge: a client and the upstream
DHCP server have to talk transparently across the rewriting layer, and we
need the FDB pre-populated by the time we forward the ACK so unicast
delivery can find the client.

Two small data structures support this:

* **XID map** – 8 entries, 30 s TTL, indexed by the DHCP transaction ID,
  storing the *real* client MAC. We use it to undo the chaddr rewrite when
  the reply arrives upstream.
* **DHCP option 61 (Client-Identifier)** – appended to outgoing requests
  with the real client MAC, so the server can distinguish multiple AP-side
  clients (which all appear to share the STA's MAC after rewriting).

### Downstream DHCP request (client → upstream server)

In `bridge_input_ap`'s IP block, when `udp->dst_port == 67`:

`snoop_dhcp_request` runs on `q` (the bridge's working copy):

1. Save the client's real `chaddr` in a local; record `(xid, real_mac)` in
   the XID map.
2. Rewrite `dhcp->chaddr` to the STA MAC. The DHCP server will see the
   request as coming from the STA's MAC.
3. Set the broadcast flag (`dhcp->flags |= 0x8000`) so the server replies as
   a broadcast – important because we cannot easily intercept a unicast
   reply addressed to a MAC the server has never seen.
4. If option 61 isn't already present, append `[61, 7, 1, real_mac]` and
   patch the IP `len`, UDP `len`, and IP checksum to reflect the 9 added
   bytes.
5. Zero the UDP checksum (legal for IPv4 UDP).

The (modified) frame is then forwarded upstream by the surrounding code via
`s_orig_lo_sta(s_sta_nif, q)`.

### Upstream DHCP reply (server → client)

In `bridge_input_sta`'s IP block, when `udp->src_port == 67 && udp->dst_port == 68`:

`snoop_dhcp_reply` runs on `q`:

1. Look up the original client MAC in the XID map by `dhcp->xid`. If found,
   rewrite `dhcp->chaddr` back to the real MAC and remember it as
   `chaddr_out`.
2. If the DHCP option 53 message type is **ACK** and `yiaddr` is non-zero,
   call `fdb_insert(yiaddr, chaddr_out)`. The lease time from option 51 is
   used as the TTL (clamped to a sane range). The FDB now knows
   `assigned-IP → client-MAC` *before* we forward the frame.
3. Zero the UDP checksum.

`bridge_input_sta` then sets the destination MAC to `chaddr_out`
(`have_dhcp` path), rewrites `eth->src` to the AP MAC, and sends the frame
to the AP via `s_orig_lo_ap`. The DHCP client on the AP side receives a
properly addressed broadcast/unicast reply, and from then on its IP is
already in the FDB so any subsequent unicast traffic from upstream finds it.

The AP-side DHCP server is explicitly not started in bridge mode (the call
to `wifi_softap_dhcps_start` is guarded by `#ifndef REPEATER_MODE`). Only
the upstream server hands out leases.

## ARP

ARP needs a different rewrite policy than IP because both sides have to
believe the L3 peer is reachable at the local interface's MAC, never the
foreign MAC.

### Downstream ARP (AP client → upstream)

In `bridge_input_ap`'s ARP block:

1. **Always learn** `arp->spa → original eth->src` into the FDB.
2. If it's an `ARP request` and the target is the STA's own IP, send a
   **proxy ARP reply** to the requester carrying the AP MAC
   (`send_proxy_arp_reply(s_ap_nif, s_orig_lo_ap, …, sta_nif->ip_addr.addr)`).
   This is the management-IP reachability path.
3. Otherwise, rewrite `arp->sha` to the STA MAC (so that upstream sees the
   STA as the sender of any ARP probe/announcement) and forward via
   `s_orig_lo_sta`.

### Upstream ARP (upstream → AP client)

In `bridge_input_sta`'s ARP block:

1. If it's an `ARP request` and the target IP is in the FDB, send a
   **proxy ARP reply** carrying the STA MAC. The upstream router learns
   "client X lives at the STA MAC" and addresses subsequent unicast frames
   to us. We then translate them back to the right AP-side MAC using the
   FDB on the way down.
2. Otherwise, rewrite `arp->sha` to the AP MAC. If the target is in the FDB,
   forward unicast to the matching client; if it's broadcast, flood to AP.

### Why proxy ARP rather than transparent ARP

The 802.11 source-MAC rule again: if we forwarded an ARP reply with the
true client MAC as `eth->src`, the upstream AP would drop it. We therefore
*always* terminate ARP at the bridge and re-issue replies with the local
side's MAC. This is the price of running a transparent IP bridge over a
WiFi link.

### Self-IP guard

Because `bridge_input_ap` learns `arp->spa → eth->src` unconditionally, the
own-IP guard in `fdb_insert` prevents a misbehaving client from poisoning
the FDB by gratuitously announcing the STA's IP for itself. Without that
guard, the management IP would resolve to a client MAC and become locally
unreachable.

## mDNS (`224.0.0.251:5353`)

mDNS has two oddities on this platform:

* **Upstream mDNS works through the SDK.** `espconn_mdns_init` (called in
  `user_main.c`) registers `esp-wifi-repeater.local` as an A record and as
  a `_http._tcp` service, listening on the STA netif. When an upstream
  device queries, the SDK responds.
* **The SDK's mDNS does not respond to AP-side queries**, even with IGMP
  joined on the AP netif and even when we deliver the frame manually to the
  STA netif's input function. The SDK filters on a WiFi-driver interface
  tag below lwip that we cannot fake. We worked around this with a small
  custom responder built into `bridge.c`.

### Downstream mDNS query (AP client → bridge / upstream)

In `bridge_input_ap`'s IP block, when `udp->dst_port == 5353`,
`handle_ap_mdns_query` is called *before* the frame is forwarded upstream.
It:

1. Validates the DNS header (QR=0, sane qdcount).
2. Walks each question label-by-label, case-insensitively matching against
   `esp-wifi-repeater.local`.
3. If a question matches and the qtype is A or ANY, builds a complete reply
   pbuf:
   * Ethernet: `dst = client MAC`, `src = AP MAC`, type = IPv4
   * IP: `src = STA IP`, `dst = client IP`, TTL 255, proto UDP
   * UDP: `src = 5353`, `dst = client UDP source port`, checksum 0
   * mDNS: QR=1, AA=1, single A answer, hostname.local, TTL 120,
     class IN with cache-flush bit, RDATA = STA IP
4. Sends the reply directly via `s_orig_lo_ap` and frees the pbuf.

The query is *also* forwarded upstream by the surrounding code (via
`s_orig_lo_sta`), which means upstream nodes still see it. So the AP-side
client may receive responses from *both* our bridge responder and any
upstream responders – which is exactly how mDNS expects to behave.

### Upstream mDNS

When an upstream device queries mDNS, the frame is multicast and arrives on
the STA radio. `bridge_input_sta` sees `is_bcast = true`, copies the frame
to the AP side via `s_orig_lo_ap`, and also passes it to `s_orig_input_sta`.
The SDK's mDNS responder picks it up and replies.

The reply is generated by lwip and routed through `bridge_output_sta`. The
multicast destination doesn't match any FDB entry, so the FDB lookup
fails – but the multicast branch builds the `01:00:5e:00:00:fb` MAC, copies
the frame to the AP side via `s_orig_lo_ap`, then lets the original
`s_orig_output_sta` deliver the same packet upstream. Both sides receive
the reply.

### Why we can't use the SDK responder for AP queries

We tried the obvious paths and none worked:

* Calling `s_orig_input_ap(p, ap_nif)` – lwip's `ip_input` rejects the
  multicast packet because the AP netif hasn't joined the group.
* `igmp_joingroup(&ap_nif->ip_addr, &mdns_grp)` so the AP netif accepts the
  packet – the multicast is now accepted by lwip but the SDK's UDP listener
  silently doesn't deliver it. The SDK appears to bind its mDNS PCB to the
  STA netif via internal means.
* Re-injecting the frame as if received on STA, by calling
  `s_orig_input_sta(p, sta_nif)` – still no response. The SDK's filter is
  below lwip and apparently keys on a tag set by the WiFi driver when the
  frame physically arrived.

Hence the small custom responder. The hostname is hard-coded to match
`user_main.c`'s `mdns.host_name = "esp-wifi-repeater"`; if you change it
there, change `MDNS_HOSTNAME` and `MDNS_HOSTNAME_LEN` in `bridge.c`
accordingly.

## Channel lock and sleep

`bridge_init` forces the soft-AP onto the same channel as the STA
(`wifi_softap_set_config` with `ap_cfg.channel = my_channel`) – an ESP8266
has a single radio and cannot bridge if the two interfaces are tuned to
different channels. It also disables WiFi modem sleep
(`wifi_set_sleep_type(NONE_SLEEP_T)`) so the AP stays continuously
available.

## Inspecting state at runtime

`bridge_show_fdb` (CLI command `show fdb` in the console) dumps the
forwarding database with TTLs:

```
Repeater FDB (IP -> Client MAC):
  192.168.178.110 -> aa:bb:cc:dd:ee:ff (expires in 583 s)
  192.168.178.112 -> 11:22:33:44:55:66 (expires in 411 s)
```

If the FDB is empty when traffic should be flowing, no DHCP ACKs and no
incoming traffic from any AP client have been seen yet – usually a sign
that the AP is on the wrong channel or a client hasn't connected yet.

## Limitations

* Single radio, single channel – the bridge is bound to the STA's current
  upstream channel.
* IPv4 only. IPv6 multicast (mDNS over `ff02::fb`, ICMPv6 ND) is not
  rewritten.
* Maximum 16 concurrent clients (FDB size). Bump `FDB_SIZE` in `bridge.c`
  if you need more.
* Only A queries for a single hard-coded hostname are answered locally on
  the AP side; SRV/PTR/TXT browsing for the bridge's own services from AP
  clients goes through whatever upstream responder picks up the relayed
  query.
* The own STA/AP IPs are barred from the FDB. If you assign multiple IPs to
  the STA netif (uncommon on this SDK), only the primary one is protected.
