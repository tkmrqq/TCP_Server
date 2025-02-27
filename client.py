import os
import socket
import re
import time

def is_valid_ip(ip):
    ip_pattern = re.compile(r'^(\d{1,3}\.){3}\d{1,3}$')
    if ip_pattern.match(ip):
        parts = ip.split('.')
        return all(0 <= int(part) <= 255 for part in parts)
    return False

def is_valid_port(port):
    return 1 <= port <= 65535

def get_user_input():
    while True:
        ip = input("Input IP-address (example: 192.168.1.4): ")
        if is_valid_ip(ip):
            break
        else:
            print("Invalid IP-address. Please, try again.")

    while True:
        try:
            port = int(input("Input port (example: 8080): "))
            if is_valid_port(port):
                break
            else:
                print("Invalid port. Please, input number in range 1-65535.")
        except ValueError:
            print("Invalid input. Input integer number.")

    return ip, port

def upload_file(filename, server_ip, port):
    if not os.path.exists(filename):
        print("Error: File does not exist!")
        return

    file_size = os.path.getsize(filename)
    print(f"Uploading file: {filename} ({file_size} bytes)")

    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        client_socket.connect((server_ip, port))
    except Exception as e:
        print(f"Connection error: {e}")
        return

    # Отправляем команду с размером файла
    command = f"UPLOAD {os.path.basename(filename)} {file_size}\n"
    client_socket.send(command.encode())

    # Ждем подтверждения "READY"
    response = client_socket.recv(1024).decode()
    print(f"Server response: {response}")
    if "READY" not in response:
        print("Server is not ready!")
        return

    # Отправляем файл поблочно
    start_time = time.time()
    sent_bytes = 0

    with open(filename, "rb") as file:
        while chunk := file.read(1024):
            client_socket.sendall(chunk)
            sent_bytes += len(chunk)
            percent = (sent_bytes / file_size) * 100
            print(f"\rProgress: {percent:.2f}% ({sent_bytes}/{file_size} bytes)", end="")

    elapsed_time = time.time() - start_time
    speed = (sent_bytes / 1024) / elapsed_time  # KB/s
    print(f"\nUpload complete! Speed: {speed:.2f} KB/s")

    # Получаем финальное подтверждение
    response = client_socket.recv(1024).decode()
    print(response)

    client_socket.close()

def download_file(filename, server_ip, port):
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        client_socket.connect((server_ip, port))
    except Exception as e:
        print(f"Connection error: {e}")
        return

    # Отправляем команду DOWNLOAD
    command = f"DOWNLOAD {filename}\n"
    client_socket.send(command.encode())

    # Получаем ответ от сервера
    response = client_socket.recv(1024).decode()
    if not response.startswith("READY"):
        print("Server error:", response)
        client_socket.close()
        return

    # Парсим размер файла
    _, file_size = response.split()
    file_size = int(file_size)
    print(f"Downloading {filename} ({file_size} bytes)...")

    # Сохраняем в папку downloads/
    save_path = os.path.join("downloads", filename)
    os.makedirs("downloads", exist_ok=True)  # Создаем папку, если её нет

    # Получение и запись файла
    start_time = time.time()
    received_bytes = 0

    with open(save_path, "wb") as file:
        while received_bytes < file_size:
            data = client_socket.recv(1024)
            if not data:
                break
            file.write(data)
            received_bytes += len(data)

            # Вывод прогресса
            percent = (received_bytes / file_size) * 100
            print(f"\rProgress: {percent:.2f}% ({received_bytes}/{file_size} bytes)", end="")

    elapsed_time = time.time() - start_time
    speed = (received_bytes / 1024) / elapsed_time  # KB/s
    print(f"\nDownload complete! Speed: {speed:.2f} KB/s")

    client_socket.close()

# Получаем IP и порт сервера
ip, port = get_user_input()
print(f"IP: {ip}, PORT: {port}")

while True:
    action = input("Choose action: [1] Upload file, [2] Download file, [0] Exit: ")
    
    if action == "1":
        file_path = input("Enter full path to the file for upload: ")
        upload_file(file_path, ip, port)
    
    elif action == "2":
        file_name = input("Enter file name to download from server: ")
        download_file(file_name, ip, port)
    
    elif action == "0":
        print("Exiting...")
        break

    else:
        print("Invalid option. Try again.")
