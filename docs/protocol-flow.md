# Nartis RF433 protocol flow (as implemented in this library)

This describes the pairing and read flows the library uses, why each step is
there, and what was learned on a real meter (Nartis D101, CC1101 on a
Raspberry Pi 5). It reflects branch `fix/dlms-invoke-id-correlation`.

## 1. Layers

| Layer | What it is | Standard? |
| --- | --- | --- |
| Radio | wM-Bus EN 13757-4 mode F framing, 2400 baud, CRC-16/DNP | Standard (link layer only) |
| Session / transport | Beacon wake, frame types, AES-128-GCM, counters, re-key | Nartis proprietary |
| Application | DLMS/COSEM GET (normal and with-list), OBIS, invoke-id | Standard (IEC 62056-5-3) |

wM-Bus only carries the bits. Everything about waking the meter, sessions and
keys is Nartis-specific. The payload of a data frame is a DLMS APDU wrapped in
an IEC 62056-47 header.

## 2. Frame types

Sent by the library (CIU):

| Type | Name | Used for |
| --- | --- | --- |
| 0x08 | BEACON | Wake, poll for data, closing beacon |
| 0x46 | DATA | Pair probe |
| 0x44 | ACK | Pair ACK; carries every DLMS GET request |
| 0x00 | PLAIN_DATA | Mode-6 confirmation after a session setup |

Received from the meter:

| Type | Name | Meaning |
| --- | --- | --- |
| 0x06 | PRESENCE_ACK | Answer to the pair probe |
| 0x40 | SHORT_ACK | Awake / request accepted, no data yet |
| 0x5B | REQUEST_ACK | Request accepted (meter already in a session) |
| 0x43 | DATA | Encrypted DLMS response |
| 0x53 | SESSION_SETUP | New data key; must be confirmed with mode-6 |

Frames that fail CRC or are addressed to another device are skipped and the
receiver listens again (`rx_expect`, up to 3 re-arms within the timeout).

## 3. Timing constants

| Constant | Value | Purpose |
| --- | --- | --- |
| rx_timeout_ms | 3000 (default) | Wait for a meter reply |
| rx_reply_timeout_ms | 900 (default) | Wait after the closing beacon |
| Pair probe retry gap | 2300 ms | Between probes |
| Pair ACK gaps | 637 ms, then 2300 ms | First ACK, later ACKs (max 4) |
| MODE6_DELAY_MS | 5629 ms | After 0x53, before mode-6 (real CIU timing) |
| MODE6_RETRY_DELAY_MS | 2300 ms | Between mode-6 attempts |
| PRIMING_POLL_BEACON_DELAY_MS | 1000 ms | Priming: gate before each poll beacon (max 2) |
| READ_POLL_BEACON_DELAY_MS | 5000 ms | Data GET: gate before each poll beacon (max 3) |
| GET_REPLY_MAX_RETRIES | 6 | Request re-sends per GET |
| GET_REJECT_RESEND_DELAY_MS | 1000 ms | Pause before re-sending after a rejected reply |
| MAX_REKEYS_PER_READ | 2 | Mid-read re-keys before the read gives up |
| Close delay | 4500 ms | From the data reply to the closing beacon (ESPHome READ_CLOSE_BEACON_DELAY_MS_) |
| Wake beacons | 4 | 1 + 3 retries |
| Wake pause after 0x43 | 4500 ms | Before the next wake beacon |

## 4. Pairing (`nartis_pair`)

```
CIU                                         Meter
 | -- 0x46 probe ('0' + serial) ----------> |   bootstrap key, counters = 0
 | <------------------------- 0x06 presence |   (retry up to 3x, 2.3 s apart)
 |   wait 637 ms
 | -- 0x44 ACK (beacon payload) ----------> |
 | <---------------- 0x53 session setup     |   (retry ACK up to 4x, 2.3 s apart;
 |   install key, reset counters            |    a missed 0x53 is re-sent by the meter)
 |   wait 5629 ms
 | -- 0x00 mode-6 ('0' + serial) ---------> |   meter commits the new key now
 | <---------------------- 0x5B / 0x40      |   (missing answer: logged, not fatal)
 |   save session
```

- The key from 0x53 is accepted only if its first 12 bytes equal the meter
  serial (ASCII). The last 4 bytes are assigned by the meter.
- If the meter answers the ACK with 0x5B/0x40 instead of 0x53, it already
  considers the CIU paired; the current key is kept.
- The CIU address comes from `--ciu-address`, else from a loaded session, else
  it is derived from `/etc/machine-id`. A meter seems to keep one active CIU
  pairing: pairing another CIU can make an older session stop working.

## 5. Read (`nartis_read`)

```
read_cycle_:
  wake_meter_        -> 0x40 / 0x5B ok; 0x53 -> complete_rekey_ -> restart
  prime_session_     -> priming GET (invoke-id N), answer discarded
  read_batches_      -> one GET per batch (invoke-id N+1, N+2, ...)
  on GET_REKEYED     -> restart from wake, keep batches already read (max 2)
save session (also on failure), radio to IDLE
```

### 5.1 Wake

Up to 4 beacons. The meter answers 0x40 (awake) or 0x5B (already in a
session). A 0x53 means it wants a new session; see 5.5.

If the last read ended less than about a minute ago, the meter may answer the
wake beacon by re-sending the last 0x43 of that read. That frame is logged and
ignored: it is not decrypted, nothing is stored and no counter moves. The next
beacon is sent 4.5 s later, and the meter then answers normally. Frames that
fail to parse are skipped within the same listen window. Worst case the wake
takes about 31 s.

### 5.2 Priming

A get-request-normal for vendor OBIS `0.0.96.128.3.255` (class 1, attr 2) is
sent on EVERY read, right after the wake. The meter treats the first GET of
each wake session as a service request whose answer may only come on the next
GET. Without priming, that late answer lands on the first real batch. The
answer is decrypted (counters advance) but never parsed or stored.

The session file still has a `session_primed` field for compatibility; it no
longer suppresses priming.

### 5.3 GET cycle (`do_get_cycle`)

```
repeat up to 6 times:
  send 0x44 with the DLMS APDU (same APDU and invoke-id on every re-send)
  reply 0x43           -> validate (5.4); accepted -> done, rejected -> wait 1 s, re-send
  reply 0x5B / 0x40    -> request accepted; up to 3 times (2 for priming):
                            wait 5 s (1 s for priming), send beacon
                            0x43 -> validate (5.4)
                            0x53 -> re-key (5.5)
  reply 0x53           -> re-key (5.5)
  nothing / other      -> re-send the request
after data: close_get_ (below)
```

Closing (`close_get_`, after every GET including priming): wait until 4.5 s
after the data reply, send a beacon, and accept as closure either a 0x40 or an
authenticated repeat of the same reply. The repeat is decrypted without moving
counters and must carry the same invoke-id and result count; it is never
stored again. Unrelated frames are ignored (at most 4, inside the 3 s window),
and no answer at all is treated as closed. On the bench the meter always
answered the closing beacon with a repeat of the data, never with 0x40.

Each batch uses get-request-with-list (up to 10 attributes, default batch 5).
A single attribute is still sent as a with-list of 1.

### 5.4 Validating a data reply

1. Nested AES-GCM decrypt with replay check (counter must be newer).
2. Parse the IEC 62056-47 wrapper and the DLMS get-response-with-list.
3. In enforce mode (`--invoke-enforce`), reject the reply when any of these
   fail:
   - the invoke-id equals the one sent (low 4 bits of byte 3 of the APDU);
   - the result count equals the number of attributes requested;
   - the IEC declared length equals the bytes present;
   - every result was parsed and no bytes are left over.
   A rejected reply clears its values and the same request is re-sent.
4. Without enforce mode the reply is parsed leniently and the invoke-id is only
   logged (`GET invoke: sent=N recv=M match=yes|NO`).
5. A per-OBIS learned type/length check (from the earlier DLMS misalignment fix)
   re-sends a batch whose value types changed unexpectedly (max 2 re-sends; a
   new type seen twice is accepted as a real change).

### 5.5 Re-key during a read (`complete_rekey_`)

When a 0x53 arrives during wake, priming or a batch:

1. Install the key (same serial check as pairing) and reset counters.
2. Wait 5629 ms, send mode-6, expect 0x5B / 0x40 (2 attempts).
3. Save the session and restart the read from the wake with a new priming GET
   and new invoke-ids. Batches already read are kept. At most 2 re-keys.

Without step 2 the meter never commits the new key and keeps re-sending 0x53,
even after minutes of rest.

### 5.6 Invoke-id

- Byte 3 of every GET request: `0xC0 | invoke_id` (high priority, confirmed
  service, 4-bit id). The id advances per logical GET (priming and each batch)
  and wraps at 16; re-sends keep the same id.
- DLMS requires the server to copy it into the response. This meter does
  (verified: `sent=2 recv=2`, `sent=3 recv=3`, ...), so it can be used to tell
  a late answer to an earlier request from the answer to the current one.
- The id restarts at 1 for every read, so it cannot identify answers from a
  previous read; priming and the replay counter cover that case.

### 5.7 Session persistence

The session file (`--session`) holds the key, meter and CIU addresses, frame
counters and sequence numbers. It is saved after pairing, after priming, after
a re-key and at the end of every read, including failed reads. A failed read
still advanced the frame counter; reloading an older value would replay
counters the meter has already seen and push it into a re-key.

## 6. Bench findings

- CC1101 frequency: the Pi 5 module needs `--freq-offset 14282` (meter carrier
  about +14.3 kHz). At offset 0, short frames worked but the long 0x53 was lost
  or failed CRC, so pairing failed. Measure each module's offset.
- Spacing between reads (enforce mode, 3 OBIS, batch size 1):

  | Gap | Before wake fix | After wake fix |
  | --- | --- | --- |
  | 120 s | 3/3 | 1/1 |
  | 60 s | 5/5 | 2/2 |
  | 30 s | 2/5 | 7/7 |

- At a 30 s gap the meter answers the first wake beacon with the last 0x43 of
  the previous read; at 60 s it does not. The close delay alone does not
  change this; the wake pause (5.1) is what fixes it.
- A read of 3 OBIS (priming plus 3 GETs) takes 52-62 s, mostly the 5 s poll
  gate and the 4.5 s close delay per GET. Larger batches cut this down.
- Stale data was only ever seen at the wake and at the closing beacon, both
  handled without storing anything. No stale reply reached a GET, so the
  invoke-id reject path has only been exercised by unit tests.
- Foreign wM-Bus frames from other devices appear regularly; they fail CRC or
  address checks and are skipped.
