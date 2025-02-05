#!/bin/bash

data_ns="nvme1n2"
dev_data="/dev/nvme1n2"
dev_meta="/dev/nvme1n4"
create()
{
	nvme zns reset -a $dev_meta
	size=14
	i=$1
	while (( i > 0 ))
	do
		echo c$size",$dev_meta" > /proc/lbz
		((i--))
	done
}
destroy()
{
	return 0
}

load()
{
	insmod $1/lbz.ko
}
unload()
{
	rmmod lbz
	clear_cache
}

format_mount_fs()
{
	echo mq-deadline > /sys/block/$data_ns/queue/scheduler

	mkfs.f2fs -i -d 1 -m -c $dev_data /dev/lbz0
	mount /dev/lbz0 /mnt/lbz0/
	echo s0,524288,525312,602112,946176 > /proc/lbz
}

collect_output()
{
	cat /proc/lbz-dev/lbz0 >> $1
}

collect_gc_info()
{
	echo -e "\ncollect gc info---------------------\n" >> $1
	ps -ef | grep lbz0_gc >> $1
	#ps -ef | grep lbz0_gc | awk '{print $2}' | xargs -I file sh -c 'dmesg|grep "file"' | grep "will"| awk -F '[ :\t=,)(]+' '{ print $4"\t"$13  }' >>$1
	ps -ef | grep lbz0_gc | awk '{print $2}' | xargs -I file sh -c 'dmesg|grep "file"' | grep "will"| awk -F '([ :\t=,)(]+)|([][ ]+)' '{ print $5"\t"$15"\t"$18"\t"$20"\t"$22"\t"$24"\t"$26"\t"$28"\t"$30 }' >> $1

	echo -e "\ncollect gc info ended---------------------\n" >> $1
}
umount_fs()
{
	#echo -e "\nbefore umount---------------------\n" >> $1
	#collect_output $1
	umount /mnt/lbz0
	echo -e "\nafter umount---------------------\n" >> $1
	collect_output $1
}

config_mem()
{
	swapoff -a
	echo 0 > /proc/sys/kernel/randomize_va_space
}

run_test()
{
	file_name=$1
	module_folder=$2
	wml_scripts=$3
	load $module_folder
	create 1
	echo "please trigger blktrace"
	sleep 10
	config_mem
	format_mount_fs
	echo -e "\nbefore filebench---------------------\n" >> $1
	collect_output $file_name
	/root/filebench-1.4.9.1/filebench -f $wml_scripts >> $file_name
	collect_output $file_name
	collect_gc_info $file_name
	umount_fs $file_name
	collect_gc_info $file_name
	destroy 1
	unload
}

for j in 2
do
	for i in "nat-sit-ssa"
	do
		local_module_folder=$i
		#run_test "0404-videoserver-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "videoserver-natsitopt.f"
		run_test "0415-varmail-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "varmail-natsitopt.f"
		run_test "0415-fileserver-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "fileserver-natsitopt.f"
		run_test "0415-webproxy-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "webproxy-natsitopt.f"
	done
done
exit 0


for j in 1
do
	for i in "ssa"
	do
		local_module_folder=$i
		run_test "0404-varmail-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "varmail-natsitopt.f"
		run_test "0404-webproxy-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "webproxy-natsitopt.f"
		run_test "0404-fileserver-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "fileserver-natsitopt.f"
	done
done

for j in 2
do
	for i in "noopt" "ssa"
	do
		local_module_folder=$i
		run_test "0404-videoserver-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "videoserver-natsitopt.f"
		run_test "0404-varmail-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "varmail-natsitopt.f"
		run_test "0404-webproxy-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "webproxy-natsitopt.f"
		run_test "0404-fileserver-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "fileserver-natsitopt.f"
	done
done

for j in 2
do
	for i in "nat-sit-ssa"
	do
		local_module_folder=$i
		run_test "0404-videoserver-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "videoserver-natsitopt.f"
		run_test "0404-varmail-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "varmail-natsitopt.f"
		run_test "0404-webproxy-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "webproxy-natsitopt.f"
		run_test "0404-fileserver-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "fileserver-natsitopt.f"
	done
done

for j in 3
do
	for i in "noopt" "ssa"
	do
		local_module_folder=$i
		run_test "0404-videoserver-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "videoserver-natsitopt.f"
		run_test "0404-varmail-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "varmail-natsitopt.f"
		run_test "0404-webproxy-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "webproxy-natsitopt.f"
		run_test "0404-fileserver-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "fileserver-natsitopt.f"
	done
done

for j in 3
do
	for i in "nat-sit-ssa"
	do
		local_module_folder=$i
		run_test "0404-videoserver-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "videoserver-natsitopt.f"
		run_test "0404-varmail-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "varmail-natsitopt.f"
		run_test "0404-webproxy-"$local_module_folder"-allmem-round"$j"-32h-4TB.res" $local_module_folder "webproxy-natsitopt.f"
		run_test "0404-fileserver-"$local_module_folder"-allmem-round"$j"-12h-4TB.res" $local_module_folder "fileserver-natsitopt.f"
	done
done
#run_test "0318-fileserver-natsitssa-allmem-round1-12h-4TB.res" "nat-sit-ssa" "videoserver-natsitopt.f"
#run_test "0319-varmail-natsitssa-allmem-round1-12h-4TB.res" "nat-sit-ssa" "varmail-natsitopt.f"
#run_test "0320-webproxy-natsitssa-allmem-round1-42h-4TB.res" "nat-sit-ssa" "webproxy-natsitopt.f"
#run_test "0321-fileserver-natsitssa-allmem-round1-12h-4TB.res" "nat-sit-ssa" "fileserver-natsitopt.f"
#run_test "0116-fileserver-natsitssa-allmem-round1-12h-4TB.res" "nat-sit-ssa" "fileserver-natsitopt.f"
