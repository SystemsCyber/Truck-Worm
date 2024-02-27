from platformio import app
from platformio.run.cli import cli as cmd_run
from platformio.device.list.util import list_serial_ports
from click.testing import CliRunner
from time import sleep
import subprocess

while True:
    try:
        serial_ports = list_serial_ports()
        print("Serial Ports: ", serial_ports)
        serial_ports = [i for i in serial_ports if "USB" in i["description"]]
        for i in serial_ports:
            # Run the cmd (pio run -t upload --upload-port <port>) and out
            result = subprocess.run(['pio', 'run', '-t', 'upload', '--upload-port', i["port"]])
        sleep(60)
    except KeyboardInterrupt:
        break