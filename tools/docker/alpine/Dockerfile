FROM docker.io/i386/alpine:3.22

ENV KERNEL="virt"
ENV ADDPKGS="cups cups-client gutenprint gutenprint-cups cups-filters ghostscript poppler-utils font-liberation dbus colord"

RUN apk add openrc alpine-base agetty alpine-conf linux-$KERNEL linux-firmware-none $ADDPKGS

RUN dbus-uuidgen --ensure
RUN rc-update add dbus default
RUN rc-update add cupsd default

COPY v86usb /usr/lib/cups/backend/v86usb
RUN chmod 700 /usr/lib/cups/backend/v86usb
RUN adduser root lpadmin

RUN sed -i 's/getty 38400 tty1/agetty --autologin root tty1 linux/' /etc/inittab
RUN echo 'ttyS0::respawn:/sbin/agetty --autologin root -s ttyS0 115200 vt100' >> /etc/inittab
RUN echo "root:" | chpasswd

RUN setup-hostname localhost

# Adding networking.sh script
RUN echo -e "rmmod ne2k-pci && modprobe ne2k-pci\nrmmod virtio-net && modprobe virtio-net\nhwclock -s\nsetup-interfaces -a -r" > /root/networking.sh && chmod +x /root/networking.sh

# https://wiki.alpinelinux.org/wiki/Alpine_Linux_in_a_chroot#Preparing_init_services
RUN for i in devfs dmesg mdev hwdrivers; do rc-update add $i sysinit; done
RUN for i in hwclock modules sysctl hostname syslog bootmisc; do rc-update add $i boot; done
RUN rc-update add killprocs shutdown

# Generate initramfs with 9p modules
RUN mkinitfs -F "base virtio 9p" $(cat /usr/share/kernel/$KERNEL/kernel.release)
