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

# Live: feed the InstrumentRouter a server order intent; it sends an iLink3 order and reconciles.
add_executable(ILink3Router "RouterTests.cpp")
target_link_libraries(ILink3Router PRIVATE ILink3Lib SecDefLib)

# Live: the whole trading-server seam — strategy wire protocol in, iLink3 out, states back.
add_executable(CmeServerTests "ServerTests.cpp")
target_link_libraries(CmeServerTests PRIVATE ServerLib)

# Live: market-data connectivity probe — join a channel's multicast feeds and report arrivals.
add_executable(Mdp3Probe "Mdp3ProbeTests.cpp")
target_link_libraries(Mdp3Probe PRIVATE Mdp3Lib ILink3Lib)

# Live: decode a market-data feed — walk packets, print messages, tally kinds and gaps.
add_executable(Mdp3Dump "Mdp3DumpTests.cpp")
target_link_libraries(Mdp3Dump PRIVATE Mdp3Lib)

# Live: one instrument's book — feed -> walker -> book builder -> market-by-price, top printed.
add_executable(Mdp3Book "Mdp3BookTests.cpp")
target_link_libraries(Mdp3Book PRIVATE Mdp3Lib SecDefLib)

# Machine: kernel-bypass runtime check — initialize TCPDirect and allocate a stack on a
# Solarflare interface (exercises the driver and license; no traffic).
add_executable(ZfProbe "ZfProbeTests.cpp")
target_link_libraries(ZfProbe PRIVATE onload_zf)

# Live: kernel-bypass TCP connect to a real address — reports established / refused / timeout.
add_executable(ZfTcp "ZfConnectionTests.cpp")
target_link_libraries(ZfTcp PRIVATE ILink3Lib onload_zf)
