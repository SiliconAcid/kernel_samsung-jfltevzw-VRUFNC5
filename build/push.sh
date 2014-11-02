mount -o remount,rw rootfs /
mount -o remount,rw sysfs /sys
mount -o remount,rw /dev 

cp /data/local/tmp/zImage /
cp /data/local/tmp/arm_kexec.ko /
cp /data/local/tmp/procfs_rw.ko /
cp /data/local/tmp/setup.sh / 
cp /data/local/tmp/kexec /
cp /data/local/tmp/msm_kexec.ko /
cp /data/local/tmp/atags / 
cp /data/local/tmp/push.sh /
cp /data/local/tmp/kexec_load.ko /
chmod 755 /setup.sh
chmod 755 /kexec
