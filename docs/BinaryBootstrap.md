# Binary session bootstrap and rendezvous

The UDP-Multiplex branch uses binary authentication and rendezvous packets.
These replace the four JSON `hello.*` frames and the JSON `jam_*` directory
protocol. There is no JSON fallback. All peers must use application wire version
5, and Jam2 Server must use the matching `J2RV` version 2 protocol. The server project is
`../Jam2-Server`; updating the client alone is insufficient.

All integers below are unsigned and big-endian. IDs, nonces, proofs and keys are
raw bytes. Hexadecimal strings remain an internal/UI representation. The existing
native audio, grid and multiplex envelopes are unchanged.

## Logical-stream authentication

Frames are `[body_length:u32][version:u8=5][kind:u8][fields]`. The body is at most
90 bytes. Before authentication, an oversized length is rejected as soon as its
four-byte prefix arrives. After authentication the existing ControlWire record
bound applies.

| Kind | Fields in order | Total bytes including length |
|---|---|---:|
| 1 Challenge | session:8, server nonce:16 | 30 |
| 2 Proof, control | client nonce:16, peer token:16, proof:16, channel:u8=1, endpoint:6, name length:u8, name:UTF-8 | 62 + name bytes |
| 2 Proof, asset | client nonce:16, peer token:16, proof:16, channel:u8=2 | 55 |
| 3 Accepted | server proof:16 | 22 |
| 4 Rejected | rejection:u8 | 7 |

Names retain the existing canonical NFC, 1?8 Unicode-letter and 32-byte UTF-8
limits. Asset authentication obtains the name from the peer's authenticated
control connection. The asset endpoint is canonically empty in the transcript;
control endpoints are canonical IPv4 text at that boundary. An endpoint on the
wire is `[address:u32][port:u16]`, with neither component zero for a control proof.

Existing fresh server/client nonces, HMAC proof domains, direction/channel key
derivation and transcript binding remain. The transcript binds wire version,
session, both nonces, peer token, channel, canonical endpoint and name. Accepted
therefore carries only its proof. Replaying a proof against a new server nonce or
changing a bound field fails authentication. Native tests exercise these cases.
Existing pending-connection caps, failed-key work limits, cancellation and timeout
ownership remain in ControlServer/ControlClient.

Rejection codes: 1 invalid fields, 2 invalid proof, 3 duplicate peer, 4 authenticated
control connection required, 5 capacity. Descriptions are rendered locally.
The authentication callback passes a typed peer name, endpoint and host through
the controller to its consumers; it no longer constructs an authentication JSON
map.

## Rendezvous UDP datagrams

Header: `[magic:4="J2RV"][version:u8=2][kind:u8]`.

Every request adds `[session:8][sender peer:16][session key:16]` (46 bytes including
header). Every response adds `[session:8][recipient peer:16]` (30 bytes including
header). Directory authorization, sender-endpoint checks, lease ordering and key
validation retain their previous semantics. The server's experimental tokenless
modes are separate and retain their own protocol.

Notation: `revision`, `sequence`, `introduction` are u64; limits/counts are u32
bounded to INT32_MAX; `attempt` is u8 in 1?10; peer IDs and nonces are 16 bytes;
`endpoint` is IPv4 u32 plus port u16; booleans are u8 exactly 0 or 1.

| Kind | Name | Additional fields in order | Datagram bytes |
|---:|---|---|---:|
| 1 | Create | revision, maximum peers, member count, nonce-present:u8, optional nonce | 63 / 79 |
| 2?4 | Join / Recover / Member | none | 46 |
| 5?6 | Pause / Leave | revision | 54 |
| 7?9 | Admit / Remove / Reject | target peer, sequence | 70 |
| 10 | Pair | nonce, peer A, peer B | 94 |
| 11 | Candidate | nonce, attempt, introduction, local endpoint | 77 |
| 12 | Ready | nonce, attempt, introduction | 71 |
| 13 | Outcome | nonce, attempt, introduction, success | 72 |
| 14 | PairAck | nonce, attempt, introduction | 71 |
| 32 | Created | revision, creator peer | 54 |
| 33 | CreateChallenge | revision, nonce | 54 |
| 34 | Paused | revision | 38 |
| 35 | Wait | creator peer | 46 |
| 36 | JoinRequest | joining peer | 46 |
| 37 | Error | error:u8, rejected kind:u8, reference:16 for kinds 7?14 | 32 / 48 |
| 38?40 | AdmitAck / RemoveAck / RejectAck | affected peer, sequence | 54 |
| 41 | Prepare | nonce, attempt, introduction, partner peer | 71 |
| 42 | Registered | nonce, attempt, introduction, partner peer, observed endpoint | 77 |
| 43 | Offer | nonce, attempt, introduction, partner peer, remote endpoint, observed endpoint, LAN route | 84 |
| 44 | Go | nonce, attempt, introduction, partner peer | 71 |
| 45?46 | Connected / Failed | nonce, attempt, introduction, partner peer, peer A, peer B | 103 |

A Candidate's all-zero local endpoint means no LAN hint; a partially zero endpoint
is invalid. Create's nonce-present byte is exactly 0 or 1. There is no general
extension map, status text, repeated creator field on each pair message, or
arbitrary error string. Sixteen bytes preserve the full peer authentication token,
not only the derived audio peer ID. Registration refreshes keep their existing
15-second interval; pending retries retain their existing timers.

Error codes: 1 credentials/creator mismatch, 2 directory capacity, 3 unavailable or
full, 4 key mismatch, 5 jam participant limit, 6 creator declined admission, 7 invalid schema. The C++ client maps these to readable local diagnostics.

Datagrams are bounded to 128 bytes. Both implementations reject unknown versions,
unknown kinds/error codes, invalid endpoints/booleans/counts/attempts, truncation
and trailing bytes. C++ signal rejection counts are exposed as
`signal_packets_rejected` in multiplex diagnostics. Authentication direction is
checked by the receiving owner: the server accepts requests; the client accepts
responses only from its configured rendezvous endpoint.

## Validation

`tests/fixtures/rendezvous-v2.tsv` and the identical server
`../Jam2-Server/src/testdata/rendezvous-v2.tsv` contain a fixed byte vector for every kind. Native C++
and Go tests independently construct typed messages and compare against those
bytes, and reject every truncated prefix. Native tests also cover transcript
binding, malformed fields, retired JSON, size bounds and connection cleanup.

Run the optimized Release `jam2_bootstrap_wire_units`,
`jam2_udp_control_transport_units`, relevant Direct/STUN/Server four-peer workflows
and `jam2_multiplex_four_session_lifecycle_integration` after changes here.
`JAM2_RENDEZVOUS_TEST_SERVER` selects the native Go executable for full four-peer
engine tests. `JAM2_RENDEZVOUS_TEST_ENDPOINT=host:port` enables
`jam2_rendezvous_server_wire_integration`, which exercises four identities and all
six introductions through a running service's real UDP receive loop, including a
Docker-published UDP endpoint.

JSON remains useful for local project persistence, automation, diagnostics and
updater/model metadata. Existing application models also still use Qt JSON values
at their internal boundaries; their network records are encoded as typed binary,
CBOR or binary patches. This change removes the remaining session-network JSON
serialization and authentication/rendezvous JSON dispatch, not those independent
local formats.

## Production signaling diagnostics

J2RV version 2 is the only current rendezvous schema. Error adds code:u8, rejected request kind:u8, then a 16-byte reference for kinds 7?14 (membership target for 7?9, introduction nonce for 10?14). Generic errors use kind zero. Error datagrams are 32 or 48 bytes. Code 2 is server capacity; code 5 is the creator participant limit; code 7 is invalid schema, including an unsupported version. Capacity suggests retrying shortly or using STUN / Direct; invalid schema suggests updating Jam2. Correlated failures cancel only their pending operation and preserve proven links. Duplicate replies produce one connection warning or startup failure. Server and client must be upgraded together; there is no older-schema decoder.
