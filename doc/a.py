import socket
import struct
import asyncio

SO_ORIGINAL_DST = 80

async def get_original_dst(sock):
    original_dst = sock.getsockopt(socket.SOL_IP, SO_ORIGINAL_DST, 16)
    port, ip = struct.unpack("!2xH4s8x", original_dst)
    ip = socket.inet_ntoa(ip)
    return ip, port

async def forward_data(source, destination, loop):
    try:
        while True:
            data = await loop.sock_recv(source, 4096)
            if not data:
                break
            await loop.sock_sendall(destination, data)
    except Exception as e:
        print(f"Error en la transferencia de datos: {e}")
    finally:
        source.close()
        destination.close()

async def handle_client(client_sock, loop):
    try:
        dst_ip, dst_port = await get_original_dst(client_sock)
        forward_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        forward_sock.connect((dst_ip, dst_port))

        # Usar asyncio para manejar la transferencia de datos de forma asíncrona
        client_to_server = asyncio.create_task(forward_data(client_sock, forward_sock, loop))
        server_to_client = asyncio.create_task(forward_data(forward_sock, client_sock, loop))

        await asyncio.gather(client_to_server, server_to_client)
    except Exception as e:
        print(f"Error al manejar el cliente: {e}")
    finally:
        client_sock.close()
        forward_sock.close()

async def start_proxy(listen_addr, listen_port):
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.bind((listen_addr, listen_port))
    server_sock.listen(5)
    print(f"Proxy escuchando en {listen_addr}:{listen_port}")

    loop = asyncio.get_event_loop()

    while True:
        client_sock, client_addr = await loop.sock_accept(server_sock)
        print(f"Conexión aceptada de {client_addr}")
        asyncio.create_task(handle_client(client_sock, loop))

if __name__ == "__main__":
    asyncio.run(start_proxy("0.0.0.0", 8080))
