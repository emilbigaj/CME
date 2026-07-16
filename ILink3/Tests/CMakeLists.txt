# iLink3 standalone test programs. Each source is its own executable, all linking ILink3Lib
# (which brings the SBE structs, config, HMAC signer, wire codec, and TCP client).

# FIXP logon signer known-answer tests (RFC 4231 HMAC-SHA256, RFC 4648 base64url,
# CME canonical message samples). Offline, deterministic.
add_executable(HmacTests "HmacTests.cpp")
target_link_libraries(HmacTests PRIVATE ILink3Lib)

# Live FIXP logon smoke test against a real gateway (Negotiate only; cannot trade).
add_executable(ILink3Logon "LogonTests.cpp")
target_link_libraries(ILink3Logon PRIVATE ILink3Lib)
