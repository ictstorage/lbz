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

prealloc_data()
{
	module_folder=$1

	if [ $module_folder"" == "nat-sit-ssa"  ]; then
		dd if=/dev/zero of=/dev/lbz0 bs=4k seek=1548288 count=2121728 oflag=direct
		echo "write 2121728 blocks for"$module_folder
		#echo s0,524288,525312,602112,946176 > /proc/lbz
	else
		#echo s0,524288,525312,602112,946176 > /proc/lbz
		dd if=/dev/zero of=/dev/lbz0 bs=4k seek=525312 count=3144704 oflag=direct
		echo "write 3144704 blocks for"$module_folder
	fi
	#if [ $module_folder"" == "ssa"  ]; then
	#	dd if=/dev/zero of=/dev/lbz0 bs=4k seek=525312 count=3144704 oflag=direct
	#elif [ $module_folder"" == "nat-sit-ssa"  ]; then
	#	dd if=/dev/zero of=/dev/lbz0 bs=4k seek=1548288 count=2121728 oflag=direct
	#fi
}

run_test()
{
	file_name=$1
	module_folder=$2
	#total_data=$((($3) * 1024 * 1024))
	inserts=$3
	dbs=$4
	db_test_mode=$5

	load $module_folder
	create 1
	config_mem
	prealloc_data $module_folder
	format_mount_fs
	echo -e "\nbefore filebench---------------------\n" >> $1
	collect_output $file_name
	#/root/filebench-1.4.9.1/filebench -f $wml_scripts >> $file_name
	#mobibench -p /mnt/lbz0/ -f 40960000 -r 4 -a 1 -t 100 >> $file_name
	echo $file_name $module_folder $inserts $dbs $db_test_mode
	#mobibench -p /mnt/nvme/ -d 0 -j 2 -n 1000000
	mobibench -p /mnt/lbz0/ -d $db_test_mode -j 2 -n $inserts  -t $dbs >> $file_name
	#mobibench -p /mnt/lbz0/ -d 1 -j 2 -n $inserts  -t $dbs >> $file_name
	#mobibench -p /mnt/lbz0/ -d 2 -j 2 -n $inserts  -t $dbs >> $file_name
	collect_output $file_name
	collect_gc_info $file_name
	umount_fs $file_name
	collect_gc_info $file_name
	destroy 1
	unload
}

for j in 1 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	#global_inserts=100000000 #2kw per thread
	global_inserts=200000 #2kw per thread
	#global_dbs=20
	global_dbs=1
	#for j in 1 2 3                                   
	for i in "noopt" "nat-sit-ssa" "ssa"
	do
		local_module_folder=$i
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-deletemode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 2
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-insertmode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 0
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-updatemode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 1
	done
done
for j in 1 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	#global_inserts=100000000 #2kw per thread
	global_inserts=500000 #2kw per thread
	#global_dbs=20
	global_dbs=1
	#for j in 1 2 3                                   
	for i in  "nat-sit-ssa" "noopt" "ssa"
	do
		local_module_folder=$i
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-insertmode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 0
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-updatemode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 1
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-deletemode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 2
	done
done
for j in 1 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	#global_inserts=100000000 #2kw per thread
	global_inserts=1000000 #2kw per thread
	#global_dbs=20
	global_dbs=1
	#for j in 1 2 3                                   
	for i in  "nat-sit-ssa" "noopt" "ssa"
	do
		local_module_folder=$i
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-insertmode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 0
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-updatemode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 1
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-deletemode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 2
	done
done

for j in 1 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	#global_inserts=100000000 #2kw per thread
	global_inserts=5000000 #2kw per thread
	#global_dbs=20
	global_dbs=1
	#for j in 1 2 3                                   
	for i in  "nat-sit-ssa" "noopt" "ssa"
	do
		local_module_folder=$i
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-insertmode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 0
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-updatemode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 1
		run_test "0313-mobi-dbtest-"$global_dbs"dbs-"$local_module_folder"-4TB-"$global_inserts"iud-deletemode-round"$j".res" $local_module_folder  $global_inserts $global_dbs 2
	done
done
exit 0
for j in 1 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	#global_inserts=100000000 #2kw per thread
	global_inserts=100000 #2kw per thread
	global_dbs=20
	#for j in 1 2 3                                   
	for i in  "noopt" 
	#for i in     "videoserver"  "varmail" "fileserver" "oltp"
	do
		run_test "0310-mobi-dbtest-"$global_dbs"dbs-"$i"-4TB-"$global_inserts"iud-round"$j".res" $i  $global_inserts $global_dbs
	done
done
exit 0
for j in 1 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	#global_inserts=100000000 #2kw per thread
	global_inserts=200000 #2kw per thread
	global_dbs=20
	#for j in 1 2 3                                   
	for i in   "nat-sit-ssa" "ssa" 
	#for i in     "videoserver"  "varmail" "fileserver" "oltp"
	do
		run_test "0309-mobi-dbtest-"$global_dbs"dbs-"$i"-4TB-"$global_inserts"iud-round"$j".res" $i  $global_inserts $global_dbs
	done
done
for j in 1 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	#global_inserts=100000000 #2kw per thread
	global_inserts=200000 #2kw per thread
	global_dbs=20
	#for j in 1 2 3                                   
	for i in  "noopt" 
	#for i in     "videoserver"  "varmail" "fileserver" "oltp"
	do
		run_test "0309-mobi-dbtest-"$global_dbs"dbs-"$i"-4TB-"$global_inserts"iud-round"$j".res" $i  $global_inserts $global_dbs
	done
done
exit 0
for j in 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	#global_inserts=100000000 #2kw per thread
	global_inserts=100000 #2kw per thread
	global_dbs=20
	#for j in 1 2 3                                   
	for i in   "nat-sit-ssa" "noopt" 
	#for i in     "videoserver"  "varmail" "fileserver" "oltp"
	do
		run_test "0308-mobi-dbtest-"$global_dbs"dbs-"$i"-4TB-"$global_inserts"iud-round"$j".res" $i  $global_inserts $global_dbs
	done
done
exit 0
for j in 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	#global_inserts=100000000 #2kw per thread
	global_inserts=100000 #2kw per thread
	global_dbs=20
	#for j in 1 2 3                                   
	for i in   "nat-sit-ssa" "noopt" 
	#for i in     "videoserver"  "varmail" "fileserver" "oltp"
	do
		run_test "0221-mobi-dbtest-"$global_dbs"dbs-"$i"-4TB-"$global_inserts"inserts-round"$j".res" $i  $global_inserts $global_dbs
	done
done
exit 0
for j in 1 2
do
	total_mem=12
	#global_total_data=800 #GB
	global_total_data=400 #GB
	global_threads=50
	#for j in 1 2 3                                   
	for i in   "nat-sit-ssa" "noopt"
	#for i in     "videoserver"  "varmail" "fileserver" "oltp"
	do
		run_test "0217-mobi-"$global_threads"threads-"$i"-round"$j"-4TB-"$global_total_data"g.res" $i  $global_total_data $global_threads
	done
done
exit 0

for j in 2 3
do
	total_mem=12
	#global_total_data=800 #GB
	global_total_data=400 #GB
	global_threads=100
	#for j in 1 2 3                                   
	for i in   "noopt" "nat-sit-ssa"
	#for i in     "videoserver"  "varmail" "fileserver" "oltp"
	do
		run_test "0217-mobi-"$global_threads"threads-"$i"-round"$j"-4TB-"$global_total_data"g.res" $i  $global_total_data $global_threads
	done
done

for j in 1 2 3
do
	total_mem=12
	global_total_data=400 #GB
	global_threads=100
	#for j in 1 2 3                                   
	for i in  "ssa"
	#for i in     "videoserver"  "varmail" "fileserver" "oltp"
	do
		wml_scripts=$i".f"
		fb_load=$i
		run_test "0217-mobi-"$global_threads"threads-"$i"-round"$j"-4TB-"$global_total_data"g.res" $i  $global_total_data $global_threads
	done
done
