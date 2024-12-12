## Local build

### Ubuntu 22.04+ install deps

```
$ sudo apt install libuv1-dev gcc make sqlite3
```

### Build & run local test

Build:
```
$ make
```

Run local tests:
```
$ make test
```

### Simulating network failures

Create the evironment for simulation:
```
sudo ip link add veth1 type veth peer name veth2

sudo ip netns add ns1
sudo ip netns add ns2

sudo ip link set veth1 netns ns1
sudo ip link set veth2 netns ns2

sudo ip netns exec ns1   ip link set veth1 up
sudo ip netns exec ns2   ip link set veth2 up
sudo ip netns exec ns1   ip addr add 10.0.0.1/24 dev veth1
sudo ip netns exec ns2   ip addr add 10.0.0.2/24 dev veth2
```

Test created configuration works well:
```
sudo ip netns exec ns1   ip a
sudo ip netns exec ns2   ip a

sudo ip netns exec ns2   ping 10.0.0.1
PING 10.0.0.1 (10.0.0.1) 56(84) bytes of data.
64 bytes from 10.0.0.1: icmp_seq=1 ttl=64 time=0.021 ms

sudo ip netns exec ns1   ping 10.0.0.2
PING 10.0.0.2 (10.0.0.2) 56(84) bytes of data.
64 bytes from 10.0.0.2: icmp_seq=1 ttl=64 time=0.014 ms
```

Test bash can run in created environment:
```
sudo nsenter -S $UID --net=/var/run/netns/ns1 bash --norc
sudo nsenter -S $UID --net=/var/run/netns/ns2 bash --norc
```

Setup link to drop packets or to delay them:
```
sudo ip netns exec ns1   tc qdisc add dev veth1 root netem delay 200ms 20ms
sudo ip netns exec ns1   tc qdisc add dev veth1 root netem loss 20%
sudo ip netns exec ns1   tc qdisc add dev veth1 root netem delay 50ms duplicate 10%
sudo ip netns exec ns1   sudo tc qdisc del dev veth1 root
```

Delete the environment:
```
sudo ip netns delete ns1
sudo ip netns delete ns2
```
