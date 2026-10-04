#!app://shell.pxe
title --optional "Pyxis"
mount --partition 2 --volume system --read-write system://
namespace create
service start text app://textfs.pxe
session app://session.pxe --configure-network --start-services
