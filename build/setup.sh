/sbin/modload
./sbin/modload
echo "0" > /proc/sys/kernel/dmesg_restrict 
echo "0" > /proc/sys/kernel/kptr_restrict
echo "1" > /sys/module/msm_watchdog/parameters/runtime_disable
echo "1" > /sys/module/msm_watchdog/parameters/runtime_disable
echo "0" > /sys/module/msm_mpdecision/enabled
echo "0" > /sys/module/msm_mpdecision/parameters/enabled

#Simple shell script to do a kexec hardboot
kexec --load-hardboot /zImage --initrd=/ramdisk-recovery.img --mem-min=0x85000000 --mem-max=0x87ffffff --command-line="console=ttyHSL0,115200,n8 androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x3F ehci-hcd.park=3"
 
kexec -e


bbx umount /sdcard
bbx umount /data
bbx umount /cache
bbx umount /ss
bbx insmod ./kexec_load.ko
bbx insmod ./procfs_rw.ko
bbx insmod ./msm_kexec.ko
bbx insmod ./arm_kexec.ko
bbx insmod ./kexec.ko
bbx sleep 2
bbx umount /firmware
bbx umount /firmware-mdm
bbx rmmod dhd


./kexec -l ./zImage --atags --atags-file=./atags --image-size=22266883 --ramdisk=./ramdisk.gz --append="console=ttyHSL0,115200,n8 androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x3F ehci-hcd.park=3 maxcpus=4 sec_log=0x100000@0xffe00008 sec_dbg=0x80000@0xfff00008 sec_debug.reset_reason=0x1a2b3c00 androidboot.warranty_bit=1 lcd_attached=1 lcd_id=0x408047 androidboot.debug_level=0x4f4c sec_debug.enable=0 sec_debug.enable_user=0 androidboot.cp_debug_level=0x55FF sec_debug.enable_cp_debug=0 cordon=c3d49e9e7ac9b61e419e29628a85d688 connie=SCH-I545_VZW_USA_1487e8a0297ae29e44069330eec54618 lpj=67678 loglevel=4 samsung.hardware=SCH-I545 androidboot.emmc_checksum=3 androidboot.warranty_bit=1 androidboot.bootloader=I545VRUFNC2 androidboot.nvdata_backup=0 androidboot.boot_recovery=0 androidboot.check_recovery_condition=0x0 level=0x574f4c44 vmalloc=450m sec_pvs=0 batt_id_value=0 androidboot.csb_val=1 androidboot.emmc=true androidboot.serialno=4f984435 androidboot.baseband=mdm"
sleep 2
./kexec -e
