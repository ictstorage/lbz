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
reset_dev()
{
	nvme zns reset -a $dev_meta
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

run_lbz_fio_test()
{
	file_name=$1
	module_folder=$2
	#fio_scripts=$3
	data_size=$3
	io_depth=$4
	rw=$5
	load $module_folder
	create 1
	config_mem
	#format_mount_fs
	collect_output $file_name
	#fio $fio_scripts --size=$data_size --output=$file_name
	echo ""
	echo $file_name $module_folder $data_size $io_depth $rw
	fio -filename=/dev/lbz0 --blocksize=4k --direct=1 --iodepth=$io_depth --readwrite=$rw --ioengine=libaio --size=$data_size --numjobs=1 --name=job1 --output=$1
	#exit 0
	collect_output $file_name
	collect_gc_info $file_name
	#destroy 1
	#unload
}
run_lbz_fio_test_read()
{
	file_name=$1
	module_folder=$2
	#fio_scripts=$3
	data_size=$3
	io_depth=$4
	rw=$5
	#format_mount_fs
	#fio $fio_scripts --size=$data_size --output=$file_name
	echo ""
	echo $file_name $module_folder $data_size $io_depth $rw
	fio -filename=/dev/lbz0 --blocksize=4k --direct=1 --iodepth=$io_depth --readwrite=$rw --ioengine=libaio --size=$data_size --numjobs=1 --name=job1 --output=$1
	collect_output $file_name
	#exit 0
	destroy 1
	unload
}

run_phy_fio_test()
{
	file_name=$1
	module_folder=$2
	#fio_scripts=$3
	data_size=$3
	io_depth=$4
	rw=$5
	reset_dev 1
	config_mem
	#format_mount_fs
	#fio $fio_scripts --size=$data_size --output=$file_name
	echo ""
	echo $file_name $module_folder $data_size $io_depth $rw
	fio -filename=/dev/nvme1n4 --blocksize=4k --direct=1 --iodepth=$io_depth --readwrite=$rw --ioengine=libaio --size=$data_size --numjobs=1 --name=job1 --output=$1
}
run_phy_fio_test_read()
{
	file_name=$1
	module_folder=$2
	#fio_scripts=$3
	data_size=$3
	io_depth=$4
	rw=$5
	#format_mount_fs
	#fio $fio_scripts --size=$data_size --output=$file_name
	echo ""
	echo $file_name $module_folder $data_size $io_depth $rw
	fio -filename=/dev/nvme1n4 --blocksize=4k --direct=1 --iodepth=$io_depth --readwrite=$rw --ioengine=libaio --size=$data_size --numjobs=1 --name=job1 --output=$1
}

for i in 1 4 8 16 32 64 128 256
do
	io_depth=$i
	for j in 1 2 3
	do
		run_lbz_fio_test "fio-lbz-1218-iodepth"$io_depth"-10g-write-round"$j".res" "noopt" "10g" $io_depth "write"
		run_lbz_fio_test_read "fio-lbz-1218-iodepth"$io_depth"-10g-read-round"$j".res" "noopt" "10g" $io_depth "read"
		run_lbz_fio_test "fio-lbz-1218-iodepth"$io_depth"-10g-randwrite-round"$j".res" "noopt" "10g" $io_depth "randwrite"
		run_lbz_fio_test_read "fio-lbz-1218-iodepth"$io_depth"-10g-randread-round"$j".res" "noopt" "10g" $io_depth "randread"
		#run_lbz_fio_test "fio-lbz-1218-iodepth"$i"-10g-write-round"$j".res" "noopt" "1m" $i "write"
		#run_lbz_fio_test "fio-lbz-1218-iodepth"$i"-10g-read-round"$j".res" "noopt" "1m" $io_depth "read"
		#run_lbz_fio_test "fio-lbz-1218-iodepth"$i"-10g-randwrite-round"$j".res" "noopt" "1m" $io_depth "randwrite"
		#run_lbz_fio_test "fio-lbz-1218-iodepth"$i"-10g-randread-round"$j".res" "noopt" "1m" $i "randread"
	done
done

for i in 1
do
	io_depth=$i
	for j in 1 2 3
	do
		run_phy_fio_test "fio-nvme1n4-1218-iodepth"$io_depth"-1g-write-round"$j".res" "noopt" "1g" $io_depth "write"
		run_phy_fio_test_read "fio-nvme1n4-1218-iodepth"$io_depth"-1g-read-round"$j".res" "noopt" "1g" $io_depth "read"
	done
done
