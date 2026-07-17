# All CME test programs live here, one executable per source, each linking the libraries it
# exercises. This folder is added after the library modules so their targets are in scope.

# FIXP logon signer known-answer tests (HMAC-SHA256, base64url, CME canonical samples). Offline.
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

# Offline: load a secdef file, report future/spread counts, list a product's front contracts.
add_executable(SecDefTests "SecDefTests.cpp")
target_link_libraries(SecDefTests PRIVATE SecDefLib)
