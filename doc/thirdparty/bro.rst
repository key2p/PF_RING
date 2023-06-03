Using Zeek with PF_RING
=======================

In order to use Zeek (formerly known as Bro) on top of pf_ring please follow this guide.

1. Install the "pfring" package (and optionally "pfring-drivers-zc-dkms"
if you want to use ZC drivers) from http://packages.ntop.org as explained
in README.apt_rpm_packages

2. Download Zeek sources from https://download.zeek.org. Please note 
that installing Zeek from package can lead to failures when capturing 
from multiple PF_RING queues. Latest tested Zeek version is 3.0.7,
newer 3.x Zeek releases are using a packet polling mechanism which is
not compatible with PF_RING ZC SPSC queues and some devices. This has
been fixed on 4.x according to https://github.com/zeek/zeek/issues/1063.

.. code-block:: console

   wget https://download.zeek.org/zeek-5.2.2.tar.gz
   tar xvzf zeek-5.2.2.tar.gz

3. Configure and install Zeek:

.. code-block:: console

   cd zeek-X.X.X
   ./configure --with-pcap=/usr/local/lib
   make && sudo make install

Please note that on some installations your should tune the LDFLAGS in order
to fix linking issues. Example:

.. code-block:: console

   LDFLAGS="-lpfring -lpcap" ./configure --with-pcap=/usr/local/

4. Make sure Zeek is correctly linked to pf_ring-aware libpcap:

.. code-block:: console

   ldd /usr/local/zeek/bin/zeek | grep pcap
           libpcap.so.1 => /usr/local/lib/libpcap.so.1 (0x00007fa371e33000)

5. Configure the node.cfg configuration file (by default located under /usr/local/zeek/etc/node.cfg) with:

.. code-block:: text

    lb_method=pf_ring 
    lb_procs=<number of processes you want to run>
    pin_cpus=<core affinity for the processes (comma-separated list)>

Example:

.. code-block:: text

   [worker-1]
   type=worker
   host=10.10.10.1
   interface=eth1
   lb_method=pf_ring
   lb_procs=8
   pin_cpus=0,1,2,3,4,5,6,7

If you are running multiple workers setting ls_procs > 1 as in the
example above, Zeek needs to setup a pf_ring kernel cluster in order
to split the traffic across the processes (otherwise your get duplicated
data). In order to force this you can append the line below to the 
configuration file (note: '99' in the example below is the cluster ID, 
feel free to replace it with any number).

.. code-block:: text

   env_vars=PCAP_PF_RING_CLUSTER_ID=99

If you installed the ZC drivers, you can configure the number of RSS queues,
as explained in the "Installing from packages" section (or running 
"ethtool -L eth1 combined <num>" as described in the "Load Balancing / RSS" section),
to the same number of processes in lb_procs, and use zc:ethX as interface name.

Example:
		
.. code-block:: text

   [worker-1]
   type=worker
   host=10.10.10.1
   interface=zc:eth1
   lb_method=pf_ring
   lb_procs=8
   pin_cpus=0,1,2,3,4,5,6,7

At this point it is possible to test the configuration running Zeek with:

.. code-block:: console

   /usr/local/zeek/bin/zeekctl
   [ZeekControl] > install
   [ZeekControl] > start

Another option for distributing the load using ZC is using zero-copy software 
distribution with zbalance_ipc. This configuration requires RSS set to single 
queue.
Run zbalance_ipc *before* running Zeek with:

.. code-block:: console

   sudo zbalance_ipc -i zc:eth1 -c 99 -n 8 -m 1 -g 8

Where:

- -c 99 is the cluster ID
- -n 8 is the number of queues
- -g 8 is core affinity for zbalance_ipc

You should use as interface name zc:<cluster id> as in the example below.

Example:

.. code-block:: text

   [worker-1]
   type=worker
   host=10.10.10.1
   interface=zc:99
   lb_method=pf_ring
   lb_procs=8
   pin_cpus=0,1,2,3,4,5,6,7

PF_RING FT Acceleration
-----------------------

In order to take advantage of the PF_RING FT L7 filtering/shunting, you also need nDPI.
Since PF_RING 7.3 *ndpi* is installed as a dependency of *pfring* when installing
from packages. If you are compiling from source code, or using an older version of PF_RING,
you need to manually install the nDPI library from https://github.com/ntop/nDPI following
the steps below:

.. code-block:: console
   
   git clone https://github.com/ntop/nDPI.git
   cd nDPI
   ./autogen.sh
   make && sudo make install

Then you need to create a configuration file with the filtering rules:

.. code-block:: console
   
   # cat /etc/pf_ring/ft-rules.conf
   [filter]
   YouTube = discard
   Netflix = discard

And set the path of the configuration file using the PF_RING_FT_CONF environment variable in your node.cfg file:

.. code-block:: text
   
   [worker-1]
   type=worker
   host=10.10.10.1
   interface=eth1
   lb_method=pf_ring
   lb_procs=8
   pin_cpus=0,1,2,3,4,5,6,7
   env_vars=PF_RING_FT_CONF=/etc/pf_ring/ft-rules.conf

At this point you are ready to run Zeek.

For further information about PF_RING FT please read http://www.ntop.org/guides/pf_ring/ft.html
