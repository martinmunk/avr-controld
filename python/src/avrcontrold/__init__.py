"""avrcontrold -- Python broker daemon for AV receivers.

Owns exactly one real connection to each receiver (NAD/Denon/Opalum) and serves
clients over a Unix socket using the same text protocol the C++ daemon spoke, so
the C ALSA plugins and the avr-cec-relay keep working unchanged. Also bridges
each receiver to Home Assistant as basic MQTT entities.
"""

__version__ = "0.4.0.dev0"
