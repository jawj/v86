FROM docker.io/i386/alpine:3.22 AS builder

COPY sane-scan.c /tmp/sane-scan.c
RUN apk add --no-cache build-base sane-dev && gcc -o /tmp/sane-scan /tmp/sane-scan.c -lsane -s -march=i686 -Os -flto


FROM docker.io/i386/alpine:3.22

ENV KERNEL="lts"
ENV ADDPKGS="usbip-utils dbus colord sane sane-backends sane-utils colord-sane"

RUN apk add openrc alpine-base agetty alpine-conf linux-$KERNEL linux-firmware-none $ADDPKGS

COPY --from=builder /tmp/sane-scan /usr/local/bin/sane-scan

# delete configs for non-USB devices, which take ages to time out 
RUN rm /etc/sane.d/dll.d/dc25 \
       /etc/sane.d/dll.d/dc210 \
       /etc/sane.d/dll.d/dc240 \
       /etc/sane.d/dll.d/canon_pp \
       /etc/sane.d/dll.d/magicolor

RUN adduser root scanner
RUN dbus-uuidgen --ensure

RUN rc-update add dbus default

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
