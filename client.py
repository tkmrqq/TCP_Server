import os
import socket
import re
import time
import struct
from threading import Thread, Lock

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
            print("Invalid IP-address. Please, try again.")

        while True:
            try:
                port = int(input("Input port (example: 8080): "))
                if self.validate_port(port):
                    break
                print("Invalid port. Please, input number in range 1-65535.")
            except ValueError:
                print("Invalid input. Input integer number.")

        while True:
            mode = input("Use UDP mode? (yes/no): ").strip().lower()
            if mode in ["yes", "no"]:
                self.udp_mode = mode == "yes"
                break
            print("Invalid choice. Type 'yes' or 'no'.")

        self.ip = ip
        self.port = port

    def send_command(self, command):
        if self.udp_mode:
            return self.send_udp_command(command)
        else:
            return self.send_tcp_command(command)

    def send_tcp_command(self, command):
        try:
            with socket.create_connection((self.ip, self.port), timeout=TCP_TIMEOUT) as sock:
                sock.sendall((command + "\n").encode())
                response = sock.recv(BUFFER_SIZE).decode().strip()
                return response
        except Exception as e:
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
                    print(f"Server error: {response}")
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
                                    print(f"\rProgress: {received/file_size*100:.1f}%", end="", flush=True)
                                    
                                else:
                                    # 9. Повторно отправляем ACK для предыдущего пакета
                                    ack_msg = f"ACK {packet_seq}".encode()
                                    sock.sendto(ack_msg, (self.ip, self.port))
                        except socket.timeout:
                            print(f"\nTimeout waiting for packet {expected_seq}, retrying...")
                            continue
                        transfered += chunk_size
                duration = time.time() - start_time
                bitrate = (file_size * 8) / duration
                print(f"\nDownload completed successfully! Bitrate: {self.format_bitrate(bitrate)}")
                return True
                
            elif mode == "upload":
                try:
                    if not os.path.exists(filename):
                        print("Error: File does not exist!")
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
                        print(f"Server error: {response.decode()}")
                        return False
                    
                    seq = 0
                    with open(filename, "rb") as file:
                        while chunk := file.read(PAYLOAD_SIZE):
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
                                        break
                                        
                                except socket.timeout:
                                    if attempt == MAX_RETRIES:
                                        print("\nMax retries reached, upload failed")
                                        return False
                            
                            seq += 1
                            print(f"\rProgress: {file.tell()/file_size*100:.1f}%", end="", flush=True)
                    
                    # Send end packet and wait for confirmation
                    end_packet = struct.pack("!IH", 0xFFFFFFFF, 0)
                    sock.sendto(end_packet, (self.ip, self.port))
                    
                    try:
                        ack, _ = sock.recvfrom(BUFFER_SIZE)
                        if "ACK 4294967295" not in ack.decode().strip():
                            print("\nWarning: Invalid final ACK")
                    except socket.timeout:
                        print("\nWarning: No final ACK received")
                    
                    duration = time.time() - start_time
                    bitrate = (transfered * 8) / duration if duration > 0 else 0
                    print(f"\nUpload complete! Bitrate: {self.format_bitrate(bitrate)}")
                    return True
                    
                except Exception as e:
                    print(f"\nTransfer failed: {str(e)}")
                    return False
        finally:
            sock.close()

    def handle_tcp_transfer(self, filename, mode):
        try:
            sock = socket.create_connection((self.ip, self.port), timeout=TCP_TIMEOUT)
            
            if mode == "download":
                sock.sendall(f"DOWNLOAD {filename}\n".encode())
                
                # Get READY response
                response = sock.recv(BUFFER_SIZE).decode().strip()
                if not response.startswith("READY"):
                    print(f"Server error: {response}")
                    return False
                
                _, file_size = response.split()
                file_size = int(file_size)
                
                # Prepare to save file
                os.makedirs("downloads", exist_ok=True)
                save_path = os.path.join("downloads", filename)
                
                with open(save_path, "wb") as file:
                    received = 0
                    while received < file_size:
                        data = sock.recv(BUFFER_SIZE)
                        if not data:
                            break
                        file.write(data)
                        received += len(data)
                        print(f"\rDownload progress: {received/file_size*100:.1f}%", end="")
                
                print("\nDownload completed successfully!")
                return True
                
            elif mode == "upload":
                # Check if file exists
                if not os.path.exists(filename):
                    print("Error: File does not exist!")
                    return False
                
                file_size = os.path.getsize(filename)
                basename = os.path.basename(filename)
                
                # Send upload request
                sock.sendall(f"UPLOAD {basename} {file_size}\n".encode())
                
                # Wait for READY response
                response = sock.recv(BUFFER_SIZE).decode().strip()
                if "READY" not in response:
                    print(f"Server error: {response}")
                    return False
                
                # Send file data
                with open(filename, "rb") as file:
                    sent = 0
                    while chunk := file.read(BUFFER_SIZE):
                        sock.sendall(chunk)
                        sent += len(chunk)
                        print(f"\rUpload progress: {sent/file_size*100:.1f}%", end="")
                
                # Get final confirmation
                response = sock.recv(BUFFER_SIZE).decode().strip()
                print(f"\n{response}")
                return True
                
        except Exception as e:
            print(f"\nTransfer failed: {str(e)}")
            return False
        finally:
            sock.close()

    def run(self):
        self.get_user_input()
        print(f"\nConnected to {self.ip}:{self.port} ({'UDP' if self.udp_mode else 'TCP'})")

        while self.running:
            print("\nOptions:")
            print("1. Upload file")
            print("2. Download file")
            print("3. Send command")
            print("0. Exit")
            
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
                    print(f"Server response: {response}")
                    
                elif choice == "0":
                    print("Exiting...")
                    self.running = False
                    
                else:
                    print("Invalid option. Please try again.")
                    
            except KeyboardInterrupt:
                print("\nInterrupted by user")
                self.running = False
            except Exception as e:
                print(f"Error: {str(e)}")

if __name__ == "__main__":
    client = FileTransferClient()
    client.run()