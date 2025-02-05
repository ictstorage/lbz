dev_path="/dev/nvme1n3"
nvme zns reset -a $dev_path
#nvme zns reset -a /dev/nvme1n6
insmod lbz.ko
#size=4096
#size=300
#size=20
#size=4
#size=24
size=14
i=$1
while (( i > 0 ))
do
	echo c$size",$dev_path" > /proc/lbz
	#echo c$size",/dev/nvme1n3"
	((i--))

done
