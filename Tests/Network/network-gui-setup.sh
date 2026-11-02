# Points the system at the test's repository, once it has an address to fetch that with.
HOST=10.0.2.2
while ! ifconfig en0 | grep -q 'inet 10.0.2.15'; do
    sleep 0.1
done
curl -sS http://$HOST:18080/ca.pem -o /tmp/ca.pem
KEY=$(curl -sS http://$HOST:18080/repository.pub)
cat > /etc/pkg/repositories.json <<EOF2
{ "repositories" : [ { "name" : "test", "url" : "https://$HOST:18443/repo", "key" : "$KEY", "certificates" : "/tmp/ca.pem" } ] }
EOF2
echo "repository: ready"
