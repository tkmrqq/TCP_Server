import os
import socket
import re
import time
import struct
from threading import Thread, Lock
from rich.console import Console
from rich.progress import Progress, BarColumn, TimeRemainingColumn
from rich.table import Table

# Инициализация консоли rich
console = Console()

BUFFER_SIZE = 1030
UDP_TIMEOUT = 2
TCP_TIMEOUT = 5
MAX_RETRIES = 5

class FileTransferClient:
    def __init__(self):
        self.ip = None
        self.port = None
        self.udp_mode = False
        self.running = True
        self.ack_lock = Lock()
        self.last_ack = None
        self.connected = False
        self.tcp_socket = None

    def validate_ip(self, ip):
        ip_pattern = re.compile(r'^(\d{1,3}\.){3}\d{1,3}$')
        return bool(ip_pattern.match(ip)) and all(0 <= int(part) <= 255 for part in ip.split('.'))

    def validate_port(self, port):
        return 1 <= port <= 65535

    def format_bitrate(self, bitrate):
        units = ["bps", "Kbps", "Mbps", "Gbps"]
        unit_idx = 0
        while bitrate > 1000 and unit_idx < 3:
            bitrate /= 1000
            unit_idx += 1
        return f"{bitrate:.2f} {units[unit_idx]}"

    def get_user_input(self):
        while True:
            ip = input("Input IP-address (example: 192.168.1.4): ")
            if self.validate_ip(ip):
                break
            console.print("❌ Invalid IP-address. Please, try again.", style="bold red")

        while True:
            try:
                port = int(input("Input port (example: 8080): "))
                if self.validate_port(port):
                    break
                console.print("❌ Invalid port. Please, input number in range 1-65535.", style="bold red")
            except ValueError:
                console.print("❌ Invalid input. Input integer number.", style="bold red")

        while True:
            mode = input("Use UDP mode? (yes/no): ").strip().lower()
            if mode in ["yes", "no"]:
                self.udp_mode = mode == "yes"
                break
            console.print("❌ Invalid choice. Type 'yes' or 'no'.", style="bold red")

        self.ip = ip
        self.port = port

    def send_command(self, command):
        if self.udp_mode:
            return self.send_udp_command(command)
        else:
            return self.send_tcp_command(command)

    def send_tcp_command(self, command):
        try:
            if not self.connected:
                self.tcp_socket = socket.create_connection((self.ip, self.port), timeout=TCP_TIMEOUT)
                self.connected = True

            self.tcp_socket.sendall((command + "\n").encode())
            response = self.tcp_socket.recv(BUFFER_SIZE).decode().strip()
            return response
        except Exception as e:
            self.connected = False
            return f"ERROR: {str(e)}"

    def send_udp_command(self, command):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.settimeout(UDP_TIMEOUT)
            sock.sendto((command + "\n").encode(), (self.ip, self.port))
            response, _ = sock.recvfrom(BUFFER_SIZE)
            return response.decode().strip()
        except Exception as e:
            return f"ERROR: {str(e)}"

    def handle_udp_transfer(self, filename, mode):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(UDP_TIMEOUT)

        try:
            start_time = time.time()
            transfered = 0

            if mode == "download":
                # 1. Отправляем запрос на загрузку
                request = f"UDP_DOWNLOAD {filename}\n".encode()
                sock.sendto(request, (self.ip, self.port))

                # 2. Получаем ответ READY с размером файла
                response, _ = sock.recvfrom(BUFFER_SIZE)
                response = response.decode().strip()

                if not response.startswith("READY"):
                    console.print(f"🛑 Server error: {response}", style="bold red")
                    return False

                _, file_size = response.split()
                file_size = int(file_size)

                # 3. Подготовка к сохранению файла
                os.makedirs("downloads", exist_ok=True)
                save_path = os.path.join("downloads", filename)

                with open(save_path, "wb") as file:
                    expected_seq = 0
                    seq = 0
                    received = 0

                    with Progress(BarColumn(), TimeRemainingColumn()) as progress:
                        task = progress.add_task("download", total=file_size)

                        while received < file_size:
                            try:
                                # 4. Получаем пакет данных
                                data, addr = sock.recvfrom(BUFFER_SIZE)

                                # 5. Проверяем пакет завершения
                                if len(data) >= 4:
                                    packet_seq = struct.unpack("!I", data[:4])[0]
                                    if packet_seq == 0xFFFFFFFF:
                                        break

                                # 6. Обрабатываем обычный пакет
                                if len(data) >= 6:
                                    packet_seq, chunk_size = struct.unpack("!IH", data[:6])
                                    chunk_data = data[6:6+chunk_size]

                                    # 7. Проверяем порядковый номер
                                    if packet_seq == expected_seq:
                                        file.write(chunk_data)
                                        received += chunk_size

                                        # 8. Немедленно отправляем подтверждение
                                        ack_msg = f"ACK {expected_seq}".encode()
                                        sock.sendto(ack_msg, (self.ip, self.port))

                                        expected_seq += 1
                                        progress.update(task, advance=chunk_size)

                                    else:
                                        # 9. Повторно отправляем ACK для предыдущего пакета
                                        ack_msg = f"ACK {packet_seq}".encode()
                                        sock.sendto(ack_msg, (self.ip, self.port))
                            except socket.timeout:
                                console.print(f"⏳ Timeout waiting for packet {expected_seq}, retrying...", style="bold yellow")
                                continue
                            transfered += chunk_size
                duration = time.time() - start_time
                bitrate = (file_size * 8) / duration
                console.print(f"🎉 Download completed successfully! Bitrate: {self.format_bitrate(bitrate)}", style="bold green")
                return True

            elif mode == "upload":
                try:
                    if not os.path.exists(filename):
                        console.print("🛑 Error: File does not exist!", style="bold red")
                        return False

                    file_size = os.path.getsize(filename)
                    basename = os.path.basename(filename)
                    start_time = time.time()
                    transfered = 0

                    # Send upload request
                    sock.sendto(f"UDP_UPLOAD {basename} {file_size}\n".encode(), (self.ip, self.port))

                    # Wait for READY response
                    response, _ = sock.recvfrom(BUFFER_SIZE)
                    if response.decode().strip() != "READY":
                        console.print(f"🛑 Server error: {response.decode()}", style="bold red")
                        return False

                    seq = 0
                    with open(filename, "rb") as file:
                        with Progress(BarColumn(), TimeRemainingColumn()) as progress:
                            task = progress.add_task("upload", total=file_size)

                            while chunk := file.read(BUFFER_SIZE):
                                header = struct.pack("!IH", seq, len(chunk))
                                packet = header + chunk

                                # Send with retries
                                for attempt in range(MAX_RETRIES + 1):
                                    sock.sendto(packet, (self.ip, self.port))

                                    try:
                                        ack, _ = sock.recvfrom(BUFFER_SIZE)
                                        ack = ack.decode().strip()

                                        if ack == f"ACK {seq}":
                                            transfered += len(chunk)
                                            progress.update(task, advance=len(chunk))
                                            break

                                    except socket.timeout:
                                        if attempt == MAX_RETRIES:
                                            console.print("🛑 Max retries reached, upload failed", style="bold red")
                                            return False

                                seq += 1

                    # Send end packet and wait for confirmation
                    end_packet = struct.pack("!IH", 0xFFFFFFFF, 0)
                    sock.sendto(end_packet, (self.ip, self.port))

                    try:
                        ack, _ = sock.recvfrom(BUFFER_SIZE)
                        if "ACK 4294967295" not in ack.decode().strip():
                            console.print("⚠️ Warning: Invalid final ACK", style="bold yellow")
                    except socket.timeout:
                        console.print("⚠️ Warning: No final ACK received", style="bold yellow")

                    duration = time.time() - start_time
                    bitrate = (transfered * 8) / duration if duration > 0 else 0
                    console.print(f"🚀 Upload complete! Bitrate: {self.format_bitrate(bitrate)}", style="bold green")
                    return True

                except Exception as e:
                    console.print(f"🛑 Transfer failed: {str(e)}", style="bold red")
                    return False
        finally:
            sock.close()

    def handle_tcp_transfer(self, filename, mode):
        try:
            if not self.connected:
                self.tcp_socket = socket.create_connection((self.ip, self.port), timeout=TCP_TIMEOUT)
                self.connected = True

            if mode == "download":
                self.tcp_socket.sendall(f"DOWNLOAD {filename}\n".encode())

                # Get READY response
                response = self.tcp_socket.recv(BUFFER_SIZE).decode().strip()
                if not response.startswith("READY"):
                    console.print(f"🛑 Server error: {response}", style="bold red")
                    return False

                _, file_size = response.split()
                file_size = int(file_size)

                # Prepare to save file
                os.makedirs("downloads", exist_ok=True)
                save_path = os.path.join("downloads", filename)

                with open(save_path, "wb") as file:
                    received = 0
                    with Progress(BarColumn(), TimeRemainingColumn()) as progress:
                        task = progress.add_task("download", total=file_size)

                        while received < file_size:
                            data = self.tcp_socket.recv(BUFFER_SIZE)
                            if not data:
                                break
                            file.write(data)
                            received += len(data)
                            progress.update(task, advance=len(data))

                console.print("🎉 Download completed successfully!", style="bold green")
                return True

            elif mode == "upload":
                # Check if file exists
                if not os.path.exists(filename):
                    console.print("🛑 Error: File does not exist!", style="bold red")
                    return False

                file_size = os.path.getsize(filename)
                basename = os.path.basename(filename)

                # Send upload request
                self.tcp_socket.sendall(f"UPLOAD {basename} {file_size}\n".encode())

                # Wait for READY response
                response = self.tcp_socket.recv(BUFFER_SIZE).decode().strip()
                if "READY" not in response:
                    console.print(f"🛑 Server error: {response}", style="bold red")
                    return False

                # Send file data
                with open(filename, "rb") as file:
                    sent = 0
                    with Progress(BarColumn(), TimeRemainingColumn()) as progress:
                        task = progress.add_task("upload", total=file_size)

                        while chunk := file.read(BUFFER_SIZE):
                            self.tcp_socket.sendall(chunk)
                            sent += len(chunk)
                            progress.update(task, advance=len(chunk))

                # Get final confirmation
                response = self.tcp_socket.recv(BUFFER_SIZE).decode().strip()
                console.print(f"🚀 {response}", style="bold green")
                return True

        except Exception as e:
            console.print(f"🛑 Transfer failed: {str(e)}", style="bold red")
            return False

    def interactive_mode(self):
        console.print("🔄 Entering interactive mode. Type 'exit' to leave.", style="bold cyan")
        while True:
            try:
                command = input("Enter command: ").strip()
                if command.lower() == "exit":
                    console.print("🔄 Leaving interactive mode.", style="bold cyan")
                    break
                response = self.send_command(command)
                console.print(f"💬 Server response: {response}", style="bold cyan")
            except KeyboardInterrupt:
                console.print("🛑 Interrupted by user", style="bold yellow")
                break
            except Exception as e:
                console.print(f"🛑 Error: {str(e)}", style="bold red")

    def run(self):
        self.get_user_input()
        console.print(f"🌐 Connected to {self.ip}:{self.port} ({'UDP' if self.udp_mode else 'TCP'})", style="bold cyan")

        while self.running:
            table = Table(title="Options", show_header=True, header_style="bold magenta")
            table.add_column("Key", style="dim", width=12)
            table.add_column("Action", justify="right")

            table.add_row("1", "Upload file")
            table.add_row("2", "Download file")
            table.add_row("3", "Send command")
            table.add_row("4", "Interactive mode")
            table.add_row("0", "Exit")

            console.print(table)

            try:
                choice = input("Select option: ").strip()

                if choice == "1":
                    file_path = input("Enter file path to upload: ").strip()
                    if self.udp_mode:
                        self.handle_udp_transfer(file_path, "upload")
                    else:
                        self.handle_tcp_transfer(file_path, "upload")

                elif choice == "2":
                    file_name = input("Enter file name to download: ").strip()
                    if self.udp_mode:
                        self.handle_udp_transfer(file_name, "download")
                    else:
                        self.handle_tcp_transfer(file_name, "download")

                elif choice == "3":
                    command = input("Enter command: ").strip()
                    response = self.send_command(command)
                    console.print(f"💬 Server response: {response}", style="bold cyan")

                elif choice == "4":
                    self.interactive_mode()

                elif choice == "0":
                    console.print("👋 Exiting...", style="bold cyan")
                    self.running = False

                else:
                    console.print("❌ Invalid option. Please try again.", style="bold red")

            except KeyboardInterrupt:
                console.print("🛑 Interrupted by user", style="bold yellow")
                self.running = False
            except Exception as e:
                console.print(f"🛑 Error: {str(e)}", style="bold red")

if __name__ == "__main__":
    client = FileTransferClient()
    client.run()
