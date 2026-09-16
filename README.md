# Synapse qPIO Home Assistant

MQTT and Home Assistant integration for AQEX Synapse qPIO modules.

## Supported devices

- `synapse_nexus` / `nexus`
- `synapse_nexus_lite` / `nexus_lite` / `lite`
- `synapse_steel` / `steel`
- `synapse_flex` / `flex`

## Build

Install the development packages for libgpiod, Mosquitto, and cJSON, then
compile the program:

```sh
gcc -std=c11 -Wall -Wextra -o src/qpio-ha src/qpio-ha.c \
	$(pkg-config --cflags --libs libgpiod libmosquitto libcjson) -lpthread
```

## Run

The program requires a JSON configuration file:

```sh
./src/qpio-ha --config src/sample-config.json
```

The MQTT loop runs in the background. For devices without inputs, the
program remains running so relay commands can be received.

## Configuration

JSON does not support comments, so the annotated example below is shown in
JSONC-style notation for documentation. Remove the comments before saving it
as JSON. `src/sample-config.json` is a ready-to-use uncommented example.

Device-specific examples are available in `src/`:

- `sample-config-nexus.json`
- `sample-config-nexus-lite.json`
- `sample-config-steel.json`
- `sample-config-flex.json`

Start one with the matching configuration, for example:

```sh
./src/qpio-ha --config src/sample-config-flex.json
```

```jsonc
{
	"device": {
		// Name displayed for the device in Home Assistant.
		"name": "AQEX Synapse Nexus (qPIO)"
	},

	// Supported values: nexus, nexus_lite, steel, or flex.
	"device_type": "synapse_nexus",

	"gpio": {
		// GPIO chip used by libgpiod. The program falls back to gpiochip4.
		"chip_path": "/dev/gpiochip0",

		// Output bank for Steel and Flex: 0 or 1.
		"dip_bank_select": 0
	},

	"mqtt": {
		"enabled": true,
		"broker": "127.0.0.1",
		"port": 1883,
		"username": "",
		"password": "",

		// Stable identifier used in MQTT and Home Assistant entity IDs.
		// Change this when configuring a second module.
		"node_id": "qpio_nexus",

		// Root for state and command topics.
		"base_topic": "qpio"
	}
}
```

`node_id` is the stable identifier for MQTT topics and Home Assistant unique
entity IDs. `device.name` is only the human-readable device name shown in Home
Assistant.

The device type can also be provided as `device.type`, or as
`gpio.device_type` for compatibility with older configurations. The root
`device_type` value is recommended.

## MQTT topics

For the default `base_topic` of `qpio`, relay commands are published to:

```text
qpio/relay/1/set
qpio/relay/2/set
qpio/relay/3/set
qpio/relay/4/set
```

Use `ON`, `OFF`, `1`, or `0` as the payload. Relay state is published to the
corresponding `qpio/relay/<number>/state` topic. Nexus and Nexus Lite input
states are published to `qpio/input/<number>/state`.

Home Assistant discovery messages are published under:

```text
homeassistant/switch/<node_id>/relay_<number>/config
homeassistant/binary_sensor/<node_id>/input_<number>/config
```

## Command-line options

```text
--config <file>       Load a JSON configuration file (required)
--mqtt-broker <host>  Override the configured MQTT broker
--mqtt-port <port>   Override the configured MQTT port
```

## Compatible hardware

- [Synapse Nexus Lite](https://www.aqex.eu/synapse-nexus-lite-raspberry-pi-io-hat.html)
