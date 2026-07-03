# nucleo-f429zi-large-http-upload

This is a Mongoose example for the STM32 Nucleo-F429ZI that runs a small HTTP
server and accepts large binary uploads. Clients can `curl` a binary body to
`POST /upload`; the request body is streamed into the board's flash instead of
being buffered in SRAM.

The example is intended for firmware update, data import, or similar workflows
where an incoming HTTP request may be larger than available RAM. The uploaded
content is stored in flash so it can be parsed or processed later. On success,
the server replies with the number of bytes stored and a CRC32 of the received
data.

To build the project, simply run `make build` command. It will initialize the folder
with the required CMSIS files for F429 and then it will build the `firmware.bin` firmware,
which can be flashed with `make flash` commmand, assuming the `STM32_Programmer_CLI` tool is
installed on your workstation.

Example:

```sh
curl -X POST --data-binary @large.bin http://<board-ip>/upload
```
