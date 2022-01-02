# Enttec DMX libftdi Example

This repository contains a simple sample code to control a USB DMX interface
on Linux using `libftdi` and `Enttec` protocol.

This code was only tested on `Eurolite USB-DMX512-PRO MK2` interface. This interface
seems to use same protocol as Enttec model.

# Implementation

A UNIX socket will accept DMX frame and this frame will be applied to a thread worker.
The thread enable slow write to the DMX device without reducing network latency.
