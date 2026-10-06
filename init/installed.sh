#!boot://shell.pxe
title --optional "Pyxis"
mount --partition 2 --volume system --read-write system://
namespace create
service start text boot://textfs.pxe
session boot://session.pxe --configure-network --start-services
