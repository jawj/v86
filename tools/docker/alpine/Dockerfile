FROM docker.io/i386/alpine:3.22

ENV KERNEL="lts"
ENV ADDPKGS="usbip-utils cups cups-client gutenprint gutenprint-cups cups-filters ghostscript poppler-utils font-liberation dbus colord"
# for SANE: "sane sane-backends sane-utils colord-sane"

RUN apk add openrc alpine-base agetty alpine-conf linux-$KERNEL linux-firmware-none $ADDPKGS

RUN adduser root lpadmin
RUN dbus-uuidgen --ensure

RUN rc-update add dbus default
RUN rc-update add cupsd default

 # static IP for USB/IP bridge to browser (/etc/network/interfaces is created in tarfilter.py)
RUN rc-update add networking default

RUN sed -i 's/getty 38400 tty1/agetty --autologin root tty1 linux/' /etc/inittab
RUN echo 'ttyS0::respawn:/sbin/agetty --autologin root -s ttyS0 115200 vt100' >> /etc/inittab
RUN echo "root:" | chpasswd

RUN setup-hostname localhost

# https://wiki.alpinelinux.org/wiki/Alpine_Linux_in_a_chroot#Preparing_init_services
RUN for i in devfs dmesg mdev hwdrivers; do rc-update add $i sysinit; done
RUN for i in hwclock modules sysctl hostname syslog bootmisc; do rc-update add $i boot; done
RUN rc-update add killprocs shutdown

# Generate initramfs with 9p modules
RUN mkinitfs -F "base virtio 9p" $(cat /usr/share/kernel/$KERNEL/kernel.release)
