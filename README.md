# AesTun
Encrypt Tun by Aes for personal use

# Example Usage
1. Install squid, disable localnet, only leave localhost, so only the programs on this cloud server can use squid without account and passward;
2. Run "nohup /root/AesTun/bin/AesTunSvr -sp 3128 >/dev/null 2>&1 &" on the squid cloud server, you can change listen port and aes key with -p and -k;
3. Run "nohup /root/AesTun/bin/AesTunCli -sh xxx.xxx.xxx.xxx -sp 6868 >/dev/null 2>&1 &", -sh and -sp are the cloud server ip and listen port; you can change local listen port and aes key with -p and -k (the key should be the same with AesTunSvr);
4. now you can use 127.0.0.1:12345 as local proxy, all the stream is encrypted on internet.

# Architecture:
![Image text](https://github.com/frankfengfl/AesTun/blob/main/AesTun.png)


