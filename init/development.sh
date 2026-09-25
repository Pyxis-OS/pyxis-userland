#!app://shell.pxe
mount --optional --read-write host
session app://session.pxe --configure-network
