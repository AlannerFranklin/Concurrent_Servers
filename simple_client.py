
import argparse
import socket
import sys
import threading
import time


def receive_bytes_loop(sock):
    while True:
        try:
            data = sock.recv(1024)
            if not data:
                print('Server disconnected')
                break
            print(f'Received: {data}')
        except Exception as e:
            print(f'Error receiving data: {e}')
            break


def main():
    argparser = argparse.ArgumentParser('Simple TCP client')
    argparser.add_argument('host', help='Server host name')
    argparser.add_argument('port', type=int, help='Server port')
    args = argparser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((args.host, args.port))
        print(f'Connected to {args.host}:{args.port}')
        
        # Start a thread to receive data from the server
        t = threading.Thread(target=receive_bytes_loop, args=(sock,))
        t.daemon = True
        t.start()

        print('Enter text to send (Ctrl+C to exit):')
        while True:
            msg = input()
            # Send the message encoded as bytes
            sock.sendall(msg.encode('utf-8'))
    except KeyboardInterrupt:
        print('\nExiting...')
    except Exception as e:
        print(f'Error: {e}')
    finally:
        sock.close()


if __name__ == '__main__':
    main()
