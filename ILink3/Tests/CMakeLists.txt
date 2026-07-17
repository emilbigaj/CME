# iLink3 standalone test programs. Each source is its own executable, all linking ILink3Lib
# (which brings the SBE structs, config, HMAC signer, wire codec, and TCP client).

# FIXP logon signer known-answer tests (RFC 4231 HMAC-SHA256, RFC 4648 base64url,
# CME canonical message samples). Offline, deterministic.
add_executable(HmacTests "HmacTests.cpp")
target_link_libraries(HmacTests PRIVATE ILink3Lib)

# Live FIXP logon smoke test against a real gateway (Negotiate + Establish; cannot trade).
add_executable(ILink3Logon "LogonTests.cpp")
target_link_libraries(ILink3Logon PRIVATE ILink3Lib)

# Live gateway test: log in and prove the keep-alive holds the session open (cannot trade).
add_executable(ILink3Gateway "GatewayTests.cpp")
target_link_libraries(ILink3Gateway PRIVATE ILink3Lib)

# Live order test: look an instrument up in the secdef, send a resting NewOrderSingle on its
# segment, and decode CME's execution-report reply.
add_executable(ILink3Order "OrderTests.cpp")
target_link_libraries(ILink3Order PRIVATE ILink3Lib SecDefLib)

# Offline: hexdump an encoded PartyDetailsDefinitionRequest to verify group framing.
add_executable(ILink3DumpParty "DumpPartyDetails.cpp")
target_link_libraries(ILink3DumpParty PRIVATE ILink3Lib)

# Live diagnostic: probe which PartyDetailsListReqID values CME accepts (id 0 vs non-zero).
add_executable(ILink3ProbeParty "ProbePartyDetails.cpp")
target_link_libraries(ILink3ProbeParty PRIVATE ILink3Lib)
