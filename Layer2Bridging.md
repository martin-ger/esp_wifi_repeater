# L2 WiFi AP-to-STA Bridge — Concepts and Implementation

## 1. The Problem: Why L2 Bridging Is Hard on a Single-Radio Device

A conventional WiFi repeater is an L3 NAT router operating at two hops: the upstream network sees only the router's STA MAC address, and every client is hidden behind a private subnet with address translation. This works well but has a fundamental limitation — clients on the AP side live in a *different subnet* from the upstream network, so their real IP addresses are invisible upstream and features like mDNS, Bonjour, UPnP, and direct device-to-device communication across the bridge do not work.

An L2 bridge solves this by forwarding raw Ethernet frames between the AP and STA interfaces, making the bridge fully transparent. Upstream devices see the clients' real MAC and IP addresses directly. The bridge itself has no IP on the forwarding path — it is, logically, an invisible switch in the cable.

The difficulty on the ESP32 is structural. Standard 802.11 infrastructure mode imposes constraints that do not exist on Ethernet:

- **Three-address frame format.** Every 802.11 data frame in infrastructure mode carries three MAC addresses: receiver (BSSID), transmitter, and final destination (or original source). When a STA transmits to an AP, the frame carries `[BSSID | STA | DA]`. When an AP transmits to a STA, the frame carries `[STA | BSSID | SA]`. A raw Ethernet bridge model (where you just swap the medium and keep MACs untouched) cannot work directly because the 802.11 MAC layer enforces that the STA's transmitter address must match the address the AP has associated.

- **Association and encryption.** The WiFi driver manages a per-association encryption context (PMK/PTK) for each STA connected to the AP. Frames injected with a foreign source MAC that has not gone through the 4-way handshake will be dropped or encrypted incorrectly.

The consequence is that a true transparent L2 bridge — where every client's real MAC goes over the air on both sides — is not achievable in standard 802.11 infrastructure mode without WDS (Wireless Distribution System / 4-address mode), which is a dedicated protocol extension not universally supported.

---

## 2. The Chosen Architecture: Software MAC Translation

This implementation takes a different approach: it operates as a **software bridge at the lwIP `netif` layer**, performing MAC address translation on every forwarded frame. The ESP32's WiFi driver continues to manage two independent 802.11 associations — one as a regular STA to the upstream AP, and one as an AP serving downstream clients — while the bridge logic sits between their respective lwIP netifs and stitches the two together by rewriting Ethernet headers.

The result is functionally transparent to IP: clients on the AP side obtain IP addresses from the upstream DHCP server and communicate on the same subnet, exactly as if they were plugged directly into the upstream network. The only non-transparent aspect is that every frame going upstream carries the ESP32 STA's MAC address as its 802.11 transmitter, which is the constraint imposed by the WiFi driver.

---

## 3. Hook Architecture

ESP-IDF exposes the lwIP `struct netif` for each WiFi interface but does not provide a public bridging API. The bridge gains control by replacing two function pointers on each netif:

- `netif->input` — called when the driver delivers a received frame upward to lwIP.
- `netif->linkoutput` — called when lwIP wants to transmit a frame downward to the driver.

Both original pointers are saved at init time and chained at the end of each hook for packets not consumed by the bridge. The internal ESP-IDF function `esp_netif_get_netif_impl()` is used to recover the raw `struct netif *` from the `esp_netif_t *` handle.

A third hook is installed for repeater mode only: `netif->output` on the STA netif. This intercepts IP-layer output — packets that lwIP itself originates (e.g., management traffic to the STA's own IP) — and reroutes them directly to an AP-side client if an FDB hit is found for the destination IP, bypassing normal ARP resolution.

The call chains are:

```
AP RX:  WiFi driver → ap_netif_input_hook → repeater_ap_rx_handle → [bridge_ap_to_sta]
                                           → original_ap_netif_input  (if not consumed)

STA RX: WiFi driver → netif_input_hook    → repeater_sta_rx_handle → [bridge_sta_to_ap]
                                           → original_netif_input     (if not consumed)

STA TX (IP-originated): lwIP → sta_netif_output_redirect → ap_netif->linkoutput (FDB hit)
                                                          → original_sta_netif_output (miss)
```

Hooks return `true` (consumed) or `false` (pass through) so the forwarding logic and the ESP32's own network stack coexist without conflict. Packets addressed to the ESP32's own AP or STA IP are passed through; everything else is bridged.

---

## 4. MAC Address Translation

### 4.1 AP → STA (upstream direction)

When a client on the AP side sends a frame, it arrives at `ap_netif_input_hook` with:
- Ethernet `src` = client's real MAC
- Ethernet `dst` = upstream destination MAC (or broadcast)

Before forwarding out the STA interface, the `src` MAC **must** be replaced with the STA interface MAC (`s_sta_netif->hwaddr`). This is the 802.11 constraint: the WiFi driver will only accept frames for transmission if the source address matches the associated STA.

For **IPv4**: the source MAC rewrite is the only change needed. The IP header is untouched — the client's real source IP travels end-to-end.

For **ARP**: two fields must be rewritten — the Ethernet frame `src` and the ARP sender hardware address (`sha`, offset 8 in the ARP body). Without the ARP body rewrite, the upstream router would learn the STA MAC from the Ethernet header but receive an ARP body claiming a different MAC, causing ARP cache inconsistency.

Simultaneously, the IP-to-client-MAC mapping is learned into the **forwarding database (FDB)** from the source IP in the IPv4 header. This is the foundation for the return path.

### 4.2 STA → AP (downstream direction)

Frames arriving from upstream have their destination set to the STA MAC — because that is what the upstream ARP cache knows. The bridge must:

1. Determine which AP-side client is the intended recipient.
2. Rewrite Ethernet `dst` to the client's real MAC.
3. Rewrite Ethernet `src` to the AP MAC (so the client sees a proper L2 frame from its associated AP).

Step 1 is the hard part. The FDB is used for unicast IPv4 — the destination IP in the IPv4 header is looked up against the table built during upstream forwarding. For ARP, the target protocol address (TPA) in the ARP body is looked up. For broadcasts, the frame is flooded to the AP side unchanged (except for the src MAC rewrite).

---

## 5. The Forwarding Database (FDB)

The FDB (`fdb.c`) is a fixed-size table of `{ ip, mac[6], expires_us }` entries, capacity 32 (configurable via `REPEATER_FDB_SIZE`). Entries are keyed by IPv4 address in network byte order. There is no secondary MAC key for lookup — only IP-to-MAC direction is needed for the downstream path.

**Learning** happens in the upstream direction: when a client sends an IPv4 packet, its source IP and MAC are extracted from the Ethernet+IP headers and inserted or refreshed with a TTL of 600 seconds (`REPEATER_FDB_DEFAULT_TTL_S`). DHCP ACK snooping adds a second learning path with the actual lease time from the DHCP ACK as the TTL, making the FDB consistent with the lease.

**Eviction** uses a simple policy: on insert, scan for a free or expired slot first; if the table is full, evict the entry with the earliest expiry timestamp (LRU-by-time).

**No MAC-learning loop prevention** is needed because the bridge is strictly asymmetric: the AP side never learns from its own forwarded frames (the `bridge_sta_to_ap` function explicitly skips frames where the source MAC is the AP MAC).

---

## 6. DHCP Handling

DHCP is the most complex special case in the bridge. The problem: DHCP uses broadcast at L2 (the server's IP may not be known), and the server reply is addressed to the client's `chaddr` (hardware address), not to the IP address — because the client does not yet have an IP. At the moment a DHCP ACK arrives from upstream, the FDB has no entry for the client's soon-to-be IP.

The solution is two-stage snooping:

**Stage 1 — XID Map** (`dhcp_xid_map.c`): When a client sends a DHCP request upstream (`DHCPREQUEST`, `DHCPDISCOVER`), the bridge intercepts it in `snoop_dhcp_client()` before forwarding. It parses the DHCP payload and records `xid → chaddr` in a transient 16-entry map with a 30-second TTL. The XID (transaction ID) is a random 32-bit number that the client embeds in every request and the server echoes in the reply.

**Stage 2 — ACK Snooping** (`snoop_dhcp_server_reply()`): When the server's reply arrives from upstream, `bridge_sta_to_ap` intercepts it before FDB lookup. The `chaddr` field in the DHCP body directly identifies the client (no XID lookup needed for routing), and if the reply is a `DHCPACK` with a non-zero `yiaddr` (offered IP), the `{ yiaddr → chaddr }` mapping is immediately inserted into the FDB with the lease time from DHCP option 51 as the TTL. This ensures the FDB has the entry ready before any unicast traffic from the server to the newly-assigned IP arrives.

The **lease map** (`dhcp_lease_map.c`) is a separate store tracking `{ chaddr → ip, hostname, lease_time }` for display and diagnostics purposes, not for forwarding.

---

## 7. Proxy ARP for the Bridge's Own Management IP

The ESP32 itself has an IP address on the STA interface (assigned by the upstream DHCP server, or statically configured). This creates an edge case: if an AP-side client sends an ARP request for the ESP32's STA IP, the normal bridge path would forward it upstream, but the ESP32's own stack would never see the reply because it would arrive on the STA interface with a destination MAC of the original requester (an AP client), not the STA MAC.

The bridge handles this with a **proxy ARP** response generated in `bridge_ap_to_sta`: if an incoming ARP request's target protocol address (TPA) matches `s_sta_netif->ip_addr`, the bridge constructs a synthetic ARP reply in-place, using the AP MAC as the reply's sender hardware address, and emits it back out the AP interface. The AP-side client learns that the ESP32's management IP is reachable at the AP MAC, so subsequent IP traffic to the management address arrives on the AP netif and is delivered to the local stack correctly.

---

## 8. What the Bridge Does Not Handle

- **Non-IP / non-ARP Ethertype frames**: Only `0x0800` (IPv4) and `0x0806` (ARP) are processed by the bridge logic. Frames with other ethertypes (IPv6 `0x86DD`, 802.1Q `0x8100`, etc.) fall through and are delivered to the local lwIP stack only.
- **IPv6**: Not bridged. The ESP32's lwIP stack processes any received IPv6 frames for itself only.
- **Multicast**: Not explicitly tracked in the FDB. Multicast frames broadcast over the AP land on the local stack but are not forwarded upstream; upstream multicast is flooded to the AP as broadcast.
- **Fragmented IP**: If a packet is fragmented, only the first fragment carries the full transport-layer port numbers. DHCP snooping requires a full DHCP header in a single fragment; fragmented DHCP (extremely rare in practice) would not be snooped.
- **True MAC transparency upstream**: The upstream network's ARP table maps every client IP to the ESP32's STA MAC, not the client's real MAC. This is the fundamental constraint imposed by 802.11 infrastructure mode. Upstream tools that correlate MACs with clients (e.g., DHCP servers using the MAC for lease identity) will see only the STA MAC for all bridge clients.
