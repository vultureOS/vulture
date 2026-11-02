# What runs on the system under test. Meta/network-test.sh's servers are on the machine that
# QEMU runs on, which is 10.0.2.2 from here; it puts the ports in /tmp through the kernel's
# boot arguments, so read them from the HTTP server it told us about.

HOST=10.0.2.2
HTTP_PORT=18080
ECHO_PORT=18081
HTTPS_GOOD=18443
HTTPS_AES256=18444
HTTPS_CHACHA=18445
HTTPS_P256=18446
HTTPS_ECDSA=18447
HTTPS_WRONG_HOST=18448
HTTPS_EXPIRED=18449
HTTPS_UNKNOWN=18450
HTTPS_TLS12=18451
HTTPS_REPO=18443

failures=0
pass() { echo "net-test: ok: $1"; }
fail() { echo "net-test: FAIL: $1"; failures=$((failures + 1)); }
# expect NAME EXPECTED ACTUAL
expect() {
    if [ "$2" = "$3" ]; then
        pass "$1"
    else
        fail "$1: wanted [$2], got [$3]"
    fi
}

# An address, from DHCP.
tries=0
while ! ifconfig en0 | grep -q 'inet 10.0.2.15'; do
    tries=$((tries + 1))
    if [ "$tries" -gt 100 ]; then
        fail "no address from DHCP"
        break
    fi
    sleep 0.1
done
ifconfig en0 | grep -q 'gateway 10.0.2.2' && pass "a router from DHCP" || fail "no router from DHCP"
grep -q 'nameserver 10.0.2.3' /etc/resolv.conf && pass "a name server from DHCP" || fail "no name server from DHCP"

# ICMP, through the NAT to the host.
ping -c 2 -q "$HOST" | grep -q '2 packets received' && pass "ping" || fail "ping"

# ---- Plain HTTP.
expect "http" "hello from the build machine" "$(curl -sS http://$HOST:$HTTP_PORT/hello)"
expect "a redirect not followed" "302" "$(curl -sS -o /dev/null -w '%{http_code}' http://$HOST:$HTTP_PORT/redirect)"
expect "a redirect followed" "hello from the build machine" "$(curl -sSL http://$HOST:$HTTP_PORT/redirect)"
curl -sSL http://$HOST:$HTTP_PORT/loop > /dev/null 2>&1
expect "redirects without end" "47" "$?"
expect "chunks" "first second third" "$(curl -sS http://$HOST:$HTTP_PORT/chunked)"
expect "gzip" "it was compressed" "$(curl -sS --compressed http://$HOST:$HTTP_PORT/gzip)"
expect "a POST" "you sent: some=data" "$(curl -sS -d some=data http://$HOST:$HTTP_PORT/post)"
expect "a HEAD" "HTTP/1.1 200 OK" "$(curl -sSI http://$HOST:$HTTP_PORT/hello | head -1 | tr -d '\r')"
expect "a 404 is an answer" "404" "$(curl -sS -o /dev/null -w '%{http_code}' http://$HOST:$HTTP_PORT/missing)"
curl -sSf http://$HOST:$HTTP_PORT/missing > /dev/null 2>&1
expect "-f makes it an error" "22" "$?"
curl -sS http://$HOST:$HTTP_PORT/short > /dev/null 2>&1
expect "a response cut short" "18" "$?"
curl -sS http://$HOST:1/ > /dev/null 2>&1
expect "a port nobody is on" "7" "$?"
curl -sS http://no-such-host.invalid/ > /dev/null 2>&1
expect "a name that doesn't exist" "6" "$?"

# ---- A large transfer: every byte counts.
curl -sS http://$HOST:$HTTP_PORT/large -o /tmp/large
expect "32 MiB by HTTP" "33554432" "$(stat -f %z /tmp/large)"
wanted=$(curl -sS http://$HOST:$HTTP_PORT/large.sha256)
expect "32 MiB intact" "$wanted" "$(shasum -a 256 /tmp/large | cut -d' ' -f1)"

# And back: all of that to an echo server and back again.
expect "echoes" "over and back" "$(echo over and back | nc -w 5 $HOST $ECHO_PORT)"
head -c 4194304 /tmp/large > /tmp/some
nc -w 10 $HOST $ECHO_PORT < /tmp/some > /tmp/echoed
expect "4 MiB there and back" "$(shasum -a 256 /tmp/some | cut -d' ' -f1)" "$(shasum -a 256 /tmp/echoed | cut -d' ' -f1)"
rm -f /tmp/some /tmp/echoed

# ---- TLS: the test's own certificate authority, from the build machine.
curl -sS http://$HOST:$HTTP_PORT/ca.pem -o /tmp/ca.pem
CA="--cacert /tmp/ca.pem"
expect "https: AES-128 with an RSA certificate" "ok" "$(curl -sS $CA https://$HOST:$HTTPS_GOOD/ok)"
expect "https: AES-256" "ok" "$(curl -sS $CA https://$HOST:$HTTPS_AES256/ok)"
expect "https: ChaCha20" "ok" "$(curl -sS $CA https://$HOST:$HTTPS_CHACHA/ok)"
expect "https: P-256, after a retry" "ok" "$(curl -sS $CA https://$HOST:$HTTPS_P256/ok)"
expect "https: an ECDSA certificate" "ok" "$(curl -sS $CA https://$HOST:$HTTPS_ECDSA/ok)"

# What must not work: each with curl's number for that.
curl -sS https://$HOST:$HTTPS_GOOD/ok > /dev/null 2>&1
expect "https: an unknown authority (the system's roots)" "60" "$?"
curl -sS $CA https://$HOST:$HTTPS_WRONG_HOST/ok > /dev/null 2>&1
expect "https: a certificate for someone else" "60" "$?"
curl -sS $CA https://$HOST:$HTTPS_EXPIRED/ok > /dev/null 2>&1
expect "https: an expired certificate" "60" "$?"
curl -sS $CA https://$HOST:$HTTPS_UNKNOWN/ok > /dev/null 2>&1
expect "https: signed by another authority" "60" "$?"
curl -sS $CA https://$HOST:$HTTPS_TLS12/ok > /dev/null 2>&1
expect "https: a server only for TLS 1.2" "35" "$?"
# -k: encrypted, but to whoever answers.
expect "https: -k gets past it" "ok" "$(curl -sSk https://$HOST:$HTTPS_UNKNOWN/ok)"

# ---- Packages, signed, over HTTPS.
# As root, as vlaunchd's jobs are. The repository's key comes from the build machine here;
# for a real one, it comes with the system.
KEY=$(curl -sS http://$HOST:$HTTP_PORT/repository.pub)
cat > /etc/pkg/repositories.json <<EOF2
{ "repositories" : [ { "name" : "test", "url" : "https://$HOST:$HTTPS_REPO/repo", "key" : "$KEY", "certificates" : "/tmp/ca.pem" } ] }
EOF2

expect "pkg: nothing yet" "" "$(pkg list)"
pkg update | grep -q '2 packages available' && pass "pkg update" || fail "pkg update"
pkg search | grep -q 'hello' && pass "pkg search" || fail "pkg search"
pkg install hello || fail "pkg install"
[ -x /Applications/Hello.app/Contents/MacOS/Hello ] && pass "pkg install put it in place" || fail "pkg install put nothing in place"
pkg list | grep -q '^hello' && pass "pkg list" || fail "pkg list"
expect "the package runs" "hello: installed and running, with no window server to show it on" "$(/Applications/Hello.app/Contents/MacOS/Hello)"
pkg info hello | grep -q '/Applications/Hello.app/Contents/Info.plist' && pass "pkg info" || fail "pkg info"
pkg remove hello || fail "pkg remove"
[ ! -e /Applications/Hello.app ] && pass "pkg remove took it away" || fail "pkg remove left it there"
expect "pkg: nothing again" "" "$(pkg list)"

# A package that isn't what the signed index says. (The server swapped it.)
pkg install tampered > /tmp/tampered.out 2>&1
expect "a tampered package is refused" "1" "$?"
grep -q "isn't what its signed index says" /tmp/tampered.out && pass "...and says why" || fail "...and says why: $(cat /tmp/tampered.out)"
[ ! -e /Applications/Tampered.app ] && pass "...and nothing was put in place" || fail "...but it was installed"

# An index that the key didn't sign.
cat > /etc/pkg/repositories.json <<EOF2
{ "repositories" : [ { "name" : "forged", "url" : "https://$HOST:$HTTPS_REPO/forged", "key" : "$KEY", "certificates" : "/tmp/ca.pem" } ] }
EOF2
pkg update > /tmp/forged.out 2>&1
expect "a forged index is refused" "1" "$?"
grep -q "isn't signed by its key" /tmp/forged.out && pass "...and says why" || fail "...and says why: $(cat /tmp/forged.out)"
expect "...and offers nothing" "" "$(pkg search)"

# An older index than before: the way a known-bad version is offered again.
cat > /etc/pkg/repositories.json <<EOF2
{ "repositories" : [ { "name" : "test", "url" : "https://$HOST:$HTTPS_REPO/old", "key" : "$KEY", "certificates" : "/tmp/ca.pem" } ] }
EOF2
pkg update > /tmp/old.out 2>&1
expect "an older index is refused" "1" "$?"

# The system's own certificate authorities parse, every one of them.
expect "the system's roots" "ok" "$(curl -sS --cacert /etc/ssl/cert.pem -k https://$HOST:$HTTPS_GOOD/ok)"

if [ "$failures" -eq 0 ]; then
    echo "net-test: PASS"
else
    echo "net-test: $failures failed"
fi
poweroff
