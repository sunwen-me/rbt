# Protocol provenance

The record size, first record anchor (`55 aa 01 01`), field order (acceleration,
gyro, Euler), float offsets, CRC location, and CRC parameters were independently
implemented from the protocol layout in
`target/hardware-ros2-reference/dm_imu/dm_imu/src/imu_driver.cpp` and
`bsp_crc.cpp`. The startup command bytes are likewise based on the reference
driver. No ROS wrapper or source implementation was copied.

The CRC is initialized to `0xffff`; each byte contributes a polynomial `0x1021`
table value selected by the current high byte XOR the input byte, followed by a
single left shift of the running CRC. The bitwise implementation is validated
against a captured 57-byte device frame and does not copy the reference table.

The reference package currently marks its license as TODO. This driver keeps
its parser and POSIX serial implementation self-contained; review the upstream
license status before distributing material copied from that reference.
