// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Decodes peer-connectivity messages that the CLIENT encoded with its own copy
// of shadnet.proto (src/shadnet/tests/peer_proto_encode.cpp in the shadPS4
// repo), and asserts every field.
//
// The two .proto files are kept in sync by hand. A field-number drift between
// them compiles cleanly on both sides and corrupts the wire silently, so the
// only test that catches it is one that crosses the boundary. Round-tripping
// through a single side's generated code would pass either way.
//
// Record framing: [u32 LE length][bytes], repeated.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "shadnet.pb.h"

namespace {

int g_failures = 0;

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    ++g_failures;
  }
  return condition;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

class RecordReader {
public:
  explicit RecordReader(std::vector<uint8_t> data) : m_data(std::move(data)) {}

  bool Next(std::string *out) {
    if (m_pos + 4 > m_data.size()) {
      return false;
    }
    const uint32_t length = static_cast<uint32_t>(m_data[m_pos]) |
                            (static_cast<uint32_t>(m_data[m_pos + 1]) << 8) |
                            (static_cast<uint32_t>(m_data[m_pos + 2]) << 16) |
                            (static_cast<uint32_t>(m_data[m_pos + 3]) << 24);
    m_pos += 4;
    if (m_pos + length > m_data.size()) {
      return false;
    }
    out->assign(reinterpret_cast<const char *>(m_data.data() + m_pos), length);
    m_pos += length;
    return true;
  }

  bool AtEnd() const { return m_pos == m_data.size(); }

private:
  std::vector<uint8_t> m_data;
  size_t m_pos = 0;
};

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: test_peer_proto_decode <fixture-file>\n";
    return 2;
  }

  std::ifstream file(argv[1], std::ios::binary);
  if (!file) {
    std::cerr << "cannot open " << argv[1] << '\n';
    return 1;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  if (data.empty()) {
    std::cerr << "fixture " << argv[1] << " is empty\n";
    return 1;
  }

  RecordReader reader(std::move(data));
  std::string record;

  CHECK(reader.Next(&record));
  {
    shadnet::PeerSessionBeginRequest msg;
    CHECK(msg.ParseFromString(record));
    CHECK(msg.target_npid() == "MintCoffeeCat");
    CHECK(msg.title_id() == "CUSA03173");
    CHECK(msg.attempt() == 7u);
  }

  CHECK(reader.Next(&record));
  {
    shadnet::PeerSessionBeginReply msg;
    CHECK(msg.ParseFromString(record));
    CHECK(msg.session_id() == 0x0123456789ABCDEFull);
    CHECK(msg.generation() == 3u);
    CHECK(msg.is_offerer());
    // Host byte order, inside 198.18.0.0/15. Swapping these two fields would
    // point each peer at its own address and is exactly what drift looks like.
    CHECK(msg.local_virtual_addr() == 0xC6120005u);
    CHECK(msg.peer_virtual_addr() == 0xC612012Au);
    CHECK(msg.peer_npid() == "connlost");
  }

  CHECK(reader.Next(&record));
  {
    shadnet::PeerSignalRequest msg;
    CHECK(msg.ParseFromString(record));
    CHECK(msg.session_id() == 0x0123456789ABCDEFull);
    CHECK(msg.generation() == 3u);
    CHECK(msg.kind() == shadnet::PEER_SIGNAL_CANDIDATE);
    CHECK(msg.payload() ==
          "a=candidate:1 1 UDP 2114977791 10.0.0.246 50497 typ host");
  }

  CHECK(reader.Next(&record));
  {
    shadnet::NotifyPeerSessionOpened msg;
    CHECK(msg.ParseFromString(record));
    CHECK(msg.session_id() == 0x0123456789ABCDEFull);
    CHECK(msg.generation() == 3u);
    CHECK(!msg.is_offerer());
    CHECK(msg.peer_npid() == "MintCoffeeCat");
    CHECK(msg.local_virtual_addr() == 0xC612012Au);
    CHECK(msg.peer_virtual_addr() == 0xC6120005u);
    CHECK(msg.title_id() == "CUSA03173");
  }

  CHECK(reader.Next(&record));
  {
    shadnet::NotifyPeerSignal msg;
    CHECK(msg.ParseFromString(record));
    CHECK(msg.session_id() == 0x0123456789ABCDEFull);
    CHECK(msg.generation() == 3u);
    CHECK(msg.kind() == shadnet::PEER_SIGNAL_GATHERING_DONE);
    CHECK(msg.payload().empty());
    CHECK(msg.from_npid() == "connlost");
  }

  CHECK(reader.Next(&record));
  {
    shadnet::NotifyPeerSessionClosed msg;
    CHECK(msg.ParseFromString(record));
    CHECK(msg.session_id() == 0x0123456789ABCDEFull);
    CHECK(msg.generation() == 3u);
    CHECK(msg.reason() == 2u);
  }

  CHECK(reader.Next(&record));
  {
    shadnet::GetIceServersReply msg;
    CHECK(msg.ParseFromString(record));
    CHECK(msg.servers_size() == 2);
    if (msg.servers_size() == 2) {
      CHECK(msg.servers(0).host() == "stun.example.org");
      CHECK(msg.servers(0).port() == 3478u);
      CHECK(!msg.servers(0).is_turn());
      CHECK(msg.servers(0).username().empty());
      CHECK(msg.servers(1).host() == "turn.example.org");
      CHECK(msg.servers(1).port() == 3479u);
      CHECK(msg.servers(1).is_turn());
      CHECK(msg.servers(1).username() == "1757552400:connlost");
      CHECK(msg.servers(1).credential() == "QUGSNM71FCId8R1W5qOaN9uMm0M=");
      CHECK(msg.servers(1).expires_at() == 1757552400ull);
    }
  }

  // Every record must have been consumed: a trailing one means the client
  // encodes something this test does not check.
  CHECK(reader.AtEnd());

  if (g_failures != 0) {
    std::cerr << "FAIL: " << g_failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "cross-implementation proto decode passed\n";
  return 0;
}
