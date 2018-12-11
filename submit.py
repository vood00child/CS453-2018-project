# coding: utf-8
###
 # @file   submit.py
 # @author Sébastien Rouault <sebastien.rouault@epfl.ch>
 #
 # @section LICENSE
 #
 # Copyright © 2018 Sébastien ROUAULT.
 #
 # This program is free software; you can redistribute it and/or modify
 # it under the terms of the GNU General Public License as published by
 # the Free Software Foundation; either version 3 of the License, or
 # any later version. Please see https://gnu.org/licenses/gpl.html
 #
 # This program is distributed in the hope that it will be useful,
 # but WITHOUT ANY WARRANTY; without even the implied warranty of
 # MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 # GNU General Public License for more details.
 #
 # @section DESCRIPTION
 #
 # Client for the automated performance measurement tool of CS-453.
###

if __name__ != "__main__":
  raise RuntimeError("Script " + repr(__file__) + " is to be used as the main module only")

# ---------------------------------------------------------------------------- #
# Python version check and imports

import sys

if sys.version_info.major < 3 or sys.version_info.minor < 5:
  print("WARNING: python interpreter not supported, please install version 3.5 or later")

import argparse
import atexit
import pathlib
import socket

# ---------------------------------------------------------------------------- #
# Configuration

version_uid = b"\x00\x00\x00\x01" # Unique version identifier (must be identical in compatible server)

default_host = "lpd48core.epfl.ch" # Default server hostname
default_port = 9997                # Default server TCP port

# ---------------------------------------------------------------------------- #
# Command line

# Description
parser = argparse.ArgumentParser(formatter_class=argparse.RawTextHelpFormatter)
parser.add_argument("--uuid",
  type=str,
  required=True,
  help="Secret user unique identifier")
parser.add_argument("--host",
  type=str,
  default=default_host,
  help="Server hostname")
parser.add_argument("--port",
  type=int,
  default=default_port,
  help="Server TCP port")
parser.add_argument("--step",
  type=int,
  default=1,
  help="Which step of the project your submission fulfills")
parser.add_argument("zippath",
  type=str,
  help="Path to a zip file containing your library code")

# Command line parsing
args = parser.parse_args(sys.argv[1:])

# ---------------------------------------------------------------------------- #
# Socket helper

def socket_encode_size(size):
  """ Encode the given number into 4 bytes.
  Args:
    size Size to encode
  Returns:
    Encoded size
  """
  # Assertions
  if size < 0 or size >= (1 << 32):
    raise OverflowError
  # Encoding
  res = bytearray(4)
  for i in range(4):
    res[i] = size & 0xFF
    size >>= 8
  return res

def socket_decode_size(size):
  """ Decode the given 4-byte encoded size into an integer.
  Args:
    size Encoded size
  Returns:
    Decoded size
  """
  # Assertions
  if len(size) != 4:
    raise OverflowError
  # Decoding
  res = 0
  for i in range(4):
    res <<= 8
    res += size[3 - i]
  return res

def socket_consume(fd, size):
  """ Repeatedly read the socket until the given size has been received.
  Args:
    fd   Socket file descriptor to read
    size Size to read from the socket
  """
  data = bytes()
  while size > 0:
    recv = fd.recv(size)
    if len(recv) <= 0:
      raise IOError("No more data in the socket")
    data += recv
    size -= len(recv)
  return data

def socket_recvfield(fd, maxsize, exact=False):
  """ Receive a field from a socket.
  Args:
    fd      Socket file descriptor to read
    maxsize Maximum/exact size to accept
    exact   Whether field size was exact
  Returns:
    Received field bytes
  """
  size = socket_decode_size(socket_consume(fd, 4))
  if size > maxsize:
    raise IOError("Field is too large")
  elif exact and size < maxsize:
    raise IOError("Field is too small")
  return socket_consume(fd, size)

def socket_sendfield(fd, data):
  """ Send a field through a socket.
  Args:
    fd   Socket file descriptor to write
    data Data bytes to send
  """
  if fd.send(socket_encode_size(len(data))) != 4 or fd.send(data) != len(data):
    raise IOError("Send failed")

# ---------------------------------------------------------------------------- #
# Prepare send buffer

zip_path = pathlib.Path(args.zippath)
if not zip_path.exists():
  print("File " + repr(str(zip_path)) + " cannot be accessed")
  exit(1)

zip_data = zip_path.read_bytes()

# ---------------------------------------------------------------------------- #
# Client

# Open connection with the server
client_fd = None
for af, socktype, proto, canonname, sa in socket.getaddrinfo(args.host, args.port, socket.AF_INET, socket.SOCK_STREAM):
  try:
    client_fd = socket.socket(af, socktype, proto)
  except OSError as msg:
    client_fd = None
    continue
  try:
    client_fd.connect(sa)
  except OSError as msg:
    client_fd.close()
    client_fd = None
    continue
  break
if client_fd is None:
  print("Unable to connect to " + str(args.host) + ":" + str(args.port))
  print("Message to the students:")
  print("  The server side is currently not running, probably because the server machine is being used for another purpose.")
  print("  Please keep calm and retry later; contact the TAs only if the problem persists for more than a few days.")
  exit(1)
atexit.register(lambda: client_fd.close())

# Check version
if socket_recvfield(client_fd, len(version_uid)) != version_uid:
  print("Protocol version mismatch with the server, please pull/download the latest version of the client.")
  exit(1)

# Send the secret user identifier
socket_sendfield(client_fd, args.uuid.encode())
res = socket_recvfield(client_fd, 1, exact=True)
msg = {1: "Invalid user secret identifier", 2: "Unknown user secret identifier", 3: "User is already logged in"}
if res[0] in msg:
  print(msg[res[0]]) # Unsuccessful identifications are logged
  exit(1)

# Send step ID
socket_sendfield(client_fd, bytes((args.step,)))

# Send zip file
socket_sendfield(client_fd, zip_data)

# Read until the socket is closed
try:
  prev = bytes()
  while True:
    data = client_fd.recv(256)
    if len(data) <= 0:
      # Here 'prev' should be empty or not enough data to decode 'prev' correctly: so do nothing
      break
    data = prev + data
    prev = bytes()
    try:
      text = data.decode()
    except UnicodeDecodeError as err:
      prev = data[err.start:]
      text = data[:err.start].decode()
    if len(text) > 0:
      sys.stdout.write(text)
      sys.stdout.flush()
except ConnectionResetError:
  pass
except KeyboardInterrupt:
  pass
