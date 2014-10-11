flowgen (Flow Generator)
========================

## Description

flowgen is simple graffic generator to xmit multiple UDP flows with 
some throughput ratio distributions of flows.

flowgen support 3 traffic distribtuion patterns.
+ same : Rate of throughput of each flow are same.
+ random : Rate of throughput of each flow are random.
+ power : Rate of throughput of each flow follows power-law.

flowgen uses Linux raw socket to transmites UDP packets. It really slow.

## Compile

	 git clone https://github.com/upa/flowgen.git
	 cd flowgen
	 make
	 ./flowgen -h

make DCE=yes is defined for ns-3-dce use.


## How to use

	 usage: ./flowgen
	 	-s : Source IP address (default 10.1.0.10)
	 	-d : Destination IP address (default 10.2.0.10)
	 	-n : Number of flows (default 10)
	 	-t : Type of flow distribution {same|random|power} (default same)
	 	-l : Packet size (defualt 1024)
	 	-f : daemon mode
	 	-r : Randomize source ports of each flows


	 % sudo ./flowgen
	 
	 or 
	 
	 % sudo ./flowgen -s 172.16.15.10 -d 172.16.12.12 -n 30 -t power -l 1500 -r -f


## Todo
	- using netmap I/O.


## Contact
upa@haeena.net
