#!/bin/sh
set -eu

if ! command -v bwrap >/dev/null ||
   ! bwrap --ro-bind / / --unshare-uts --hostname mill /bin/true >/dev/null 2>&1; then
  echo "DNS fixture requires bubblewrap with hostname namespaces"
  exit 77
fi

fixture=$(mktemp -d "$2/remote-server-dns.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
cat > "$fixture/hosts" <<'EOF'
127.0.0.1 localhost
::1 mill
192.0.2.35 mill.other.test.
127.0.0.1 mill.workshop.test.
192.0.2.36 mill.workshop.test.
::1 mill.ipv6.test.
192.0.2.39 mill.delayed.test.
EOF
printf '%s\n' 'hosts: files' > "$fixture/nsswitch.conf"

test_binary=$1
if command -v ip >/dev/null &&
   bwrap --unshare-user --unshare-net --cap-add CAP_NET_ADMIN --ro-bind / / \
     ip link set lo up >/dev/null 2>&1; then
  delayed=mill.delayed.test
  set -- --unshare-user --unshare-net --cap-add CAP_NET_ADMIN
else
  delayed=
  set --
fi

bwrap "$@" --ro-bind / / --dev-bind /dev /dev --proc /proc --unshare-uts --hostname mill \
  --setenv LOCALDOMAIN 'other.test workshop.test ipv6.test delayed.test' --setenv RESOLV_MULTI on \
  --ro-bind "$fixture/hosts" /etc/hosts --ro-bind "$fixture/nsswitch.conf" /etc/nsswitch.conf \
  --bind "$fixture" "$fixture" /bin/sh -ec '
    if test -n "$3"; then ip link set lo up; fi
    # Ensure the fixture really returns both IPv4 addresses for the same name.
    addresses=$(getent --no-addrconfig ahostsv4 mill.workshop.test. | awk '\''{print $1}'\'' | sort -u)
    test "$addresses" = "$(printf "127.0.0.1\n192.0.2.36\n")"
    exec "$1" "$2" mill.workshop.test mill.other.test mill.ipv6.test "$3"
  ' sh "$test_binary" "$fixture" "$delayed"
