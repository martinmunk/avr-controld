#!/usr/bin/env python3
"""
Mock Denon AVR Server for Testing

This mock server implements the Denon IP protocol for testing purposes.
It listens on TCP port 23 (or specified port) and responds to Denon commands.

Usage:
    python3 denon_mock_server.py [--port PORT] [--host HOST] [--max-volume MAX]

Example:
    python3 denon_mock_server.py --port 23 --max-volume 70
"""

import socket
import threading
import argparse
import time
import sys
from typing import Dict, Optional

class DenonMockReceiver:
    """Simulates a Denon AVR receiver's state and behavior"""

    def __init__(self, max_volume: int = 98):
        self.volume = 50
        self.max_volume = max_volume
        self.muted = False
        self.power_on = True
        self.current_input = "MPLAY"
        self.speaker_levels: Dict[str, int] = {
            "FL": 50, "FR": 50, "C": 50, "SW": 50,
            "SL": 50, "SR": 50, "SBL": 50, "SBR": 50,
            "SB": 50, "FHL": 50, "FHR": 50
        }
        self.lock = threading.Lock()

    def process_command(self, command: str) -> Optional[str]:
        """Process a Denon command and return the response"""
        command = command.strip().upper()

        with self.lock:
            # Volume commands
            if command == "MV?":
                # Return volume and max volume
                return f"MV{self.volume:02d}\rMVMAX {self.max_volume}\r"

            elif command.startswith("MV") and len(command) > 2:
                if command == "MVUP":
                    if self.volume < self.max_volume:
                        self.volume += 1
                    return f"MV{self.volume:02d}\r"
                elif command == "MVDOWN":
                    if self.volume > 0:
                        self.volume -= 1
                    return f"MV{self.volume:02d}\r"
                else:
                    # Set volume (MV45 or MV385 for 38.5)
                    try:
                        vol_str = command[2:]
                        if len(vol_str) == 2:
                            vol = int(vol_str)
                        elif len(vol_str) == 3:
                            vol = int(vol_str) // 10  # 385 -> 38
                        else:
                            return None

                        if 0 <= vol <= self.max_volume:
                            self.volume = vol
                            return f"MV{self.volume:02d}\r"
                    except ValueError:
                        return None

            # Mute commands
            elif command == "MU?":
                return f"MU{'ON' if self.muted else 'OFF'}\r"
            elif command == "MUON":
                self.muted = True
                return "MUON\r"
            elif command == "MUOFF":
                self.muted = False
                return "MUOFF\r"

            # Power commands
            elif command == "PW?":
                return f"PW{'ON' if self.power_on else 'STANDBY'}\r"
            elif command == "PWON":
                self.power_on = True
                return "PWON\r"
            elif command == "PWSTANDBY":
                self.power_on = False
                return "PWSTANDBY\r"

            # Input commands
            elif command == "SI?":
                return f"SI{self.current_input}\r"
            elif command.startswith("SI") and len(command) > 2:
                input_name = command[2:]
                self.current_input = input_name
                return f"SI{self.current_input}\r"

            # Speaker level commands
            elif command.startswith("SSLEV"):
                parts = command[5:].split()
                if len(parts) == 2:
                    speaker, level_str = parts
                    try:
                        level = int(level_str)
                        if speaker in self.speaker_levels and 38 <= level <= 62:
                            self.speaker_levels[speaker] = level
                            return f"SSLEV{speaker} {level}\r"
                    except ValueError:
                        return None
                elif len(parts) == 1:
                    # Query speaker level
                    speaker = parts[0]
                    if speaker in self.speaker_levels:
                        level = self.speaker_levels[speaker]
                        return f"SSLEV{speaker} {level}\r"

            # Query available inputs (slow on real hardware, return subset)
            elif command == "SSFUN ?":
                return "SSFUN SAT/CBL\rSSFUN DVD\rSSFUN BD\rSSFUN MPLAY\r"

        return None


class DenonMockServer:
    """Mock server that handles multiple client connections"""

    def __init__(self, host: str = "0.0.0.0", port: int = 23, max_volume: int = 98):
        self.host = host
        self.port = port
        self.receiver = DenonMockReceiver(max_volume)
        self.running = False
        self.server_socket: Optional[socket.socket] = None
        self.client_threads = []

    def start(self):
        """Start the mock server"""
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

        try:
            self.server_socket.bind((self.host, self.port))
            self.server_socket.listen(5)
            self.running = True

            print(f"Denon Mock Server listening on {self.host}:{self.port}")
            print(f"Max volume set to: {self.receiver.max_volume}")
            print("Press Ctrl+C to stop")
            print()

            while self.running:
                try:
                    self.server_socket.settimeout(1.0)
                    client_socket, client_address = self.server_socket.accept()
                    print(f"[{time.strftime('%H:%M:%S')}] Client connected: {client_address}")

                    # Handle client in a new thread
                    client_thread = threading.Thread(
                        target=self.handle_client,
                        args=(client_socket, client_address),
                        daemon=True
                    )
                    client_thread.start()
                    self.client_threads.append(client_thread)

                except socket.timeout:
                    continue
                except OSError:
                    if self.running:
                        raise

        except KeyboardInterrupt:
            print("\nShutting down...")
        finally:
            self.stop()

    def handle_client(self, client_socket: socket.socket, client_address):
        """Handle a single client connection"""
        try:
            buffer = ""

            while self.running:
                try:
                    data = client_socket.recv(1024).decode('ascii', errors='ignore')
                    if not data:
                        break

                    buffer += data

                    # Process complete commands (terminated by \r)
                    while '\r' in buffer:
                        command, buffer = buffer.split('\r', 1)
                        command = command.strip()

                        if command:
                            print(f"[{time.strftime('%H:%M:%S')}] {client_address} -> {command}")

                            response = self.receiver.process_command(command)
                            if response:
                                client_socket.sendall(response.encode('ascii'))
                                # Log response without the \r for readability
                                log_response = response.replace('\r', ' | ').strip(' | ')
                                print(f"[{time.strftime('%H:%M:%S')}] {client_address} <- {log_response}")

                except socket.timeout:
                    continue
                except (ConnectionResetError, BrokenPipeError):
                    break

        except Exception as e:
            print(f"Error handling client {client_address}: {e}")
        finally:
            print(f"[{time.strftime('%H:%M:%S')}] Client disconnected: {client_address}")
            client_socket.close()

    def stop(self):
        """Stop the mock server"""
        self.running = False
        if self.server_socket:
            self.server_socket.close()


def main():
    parser = argparse.ArgumentParser(description="Mock Denon AVR Server for Testing")
    parser.add_argument("--host", default="0.0.0.0", help="Host to bind to (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=23, help="Port to listen on (default: 23)")
    parser.add_argument("--max-volume", type=int, default=98,
                       help="Maximum volume setting (default: 98)")

    args = parser.parse_args()

    if args.max_volume < 1 or args.max_volume > 98:
        print("Error: max-volume must be between 1 and 98")
        sys.exit(1)

    server = DenonMockServer(host=args.host, port=args.port, max_volume=args.max_volume)
    server.start()


if __name__ == "__main__":
    main()
