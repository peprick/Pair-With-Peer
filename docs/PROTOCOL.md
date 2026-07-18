# Pair with Peer wire protocol

Pair with Peer uses a compact length-prefixed TCP protocol. This document describes the current development protocol; compatibility is not guaranteed before a stable release.

## Encoding

- Unsigned 32-bit and 64-bit integers use network byte order (big endian).
- Booleans are one byte: `0` or `1`.
- Strings are a 32-bit byte length followed by UTF-8 bytes, capped at 4 KiB.
- File content is a 64-bit byte length followed by exactly that many bytes.
- A peer endpoint is a string host followed by a 32-bit port value.

Every TCP read and write is completed in a loop, so packet boundaries do not affect message framing.

## Tracker requests

| ID | Name | Request | Response |
| ---: | --- | --- | --- |
| 1 | Register | username, endpoint | accepted boolean |
| 2 | Find file | requester, filename | found boolean; if found, username and endpoint |
| 3 | List files | requester | peer count, then each peer name and its file names |
| 4 | Peer info | username | found boolean; if found, endpoint |
| 5 | Unregister | username, endpoint | none |

## Peer requests

| ID | Name | Request | Response |
| ---: | --- | --- | --- |
| 1 | Has file | filename | available boolean |
| 2 | Fetch file | filename | available boolean, then file content when available |
| 3 | List files | none | file count, then file names |
| 4 | Offer file | sender, filename | accepted boolean, file content, completed boolean |

Peers reject offer-file requests unless the receiving process was started with `--accept-direct`.

## Trust model

The tracker coordinates discovery but does not relay file content. The protocol currently provides no identity proof, authorization, encryption, or cryptographic integrity. It is intended for educational use on trusted networks.
