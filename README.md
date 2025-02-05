# LBZ: A Lightweight Block Device for Supporting F2FS on ZNS SSD
This is the source code of the block device prototype proposed in our ICCD 2024 paper **LBZ: A Lightweight Block Device for Supporting F2FS on ZNS SSD**. You can get this paper using this link: https://ieeexplore.ieee.org/abstract/document/10818063

## 代码说明
src目录：lbz基础版本，默认支持将所有用户写转换为对ZNS SSD的顺序写。可以编译选项的方式支持针对F2FS的各类优化。CONFIG_LBZ_NAT_SIT_SUPPORT选项代表LBZ可感知F2FS的元数据布局；CONFIG_LBZ_NAT_SIT_FREE_SUPPORT代表在F2FS元数据下刷完成后LBZ可将NAT和SIT区域的一半空间标识为无效；CONFIG_LBZ_NAT_SIT_STREAM_SUPPORT代表LBZ将F2FS不同区域的数据块放置在ZNS SSD的不同zone中。

tests目录：自动化脚本

## 测试环境
代码运行测试使用的内核和ZNS SSD如下:
1. 内核：Fedora 35，内核 5.14.10-300.fc35.x86_64
2. ZNS SSD: Western Digital Ultrastar DC ZN540

## 设备创建
参考代码中的create.sh脚本
	
## 设备删除：
rmmod lbz

## 自动化脚本说明
filebench测试脚本：filebench-test/auto-fb-test.sh文件

SQLite负载测试脚本：sqlite-test/auto-mobi-db-test-iud.sh文件

fio测试脚本：auto-fio-test.sh

## 联系方式
If you have any question about the open-sourced code, please feel free to contact: jiangdejun@ict.ac.cn 
