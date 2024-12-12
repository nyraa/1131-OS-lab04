#!/bin/bash

# Check if the correct number of arguments is provided
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <byte_size>"
    exit 1
fi

# Parameters
BYTE_SIZE=$1

# Validate that BYTE_SIZE is a positive integer
if ! [[ "$BYTE_SIZE" =~ ^[0-9]+$ ]] || [ "$BYTE_SIZE" -le 0 ]; then
    echo "Error: byte_size must be a positive integer."
    exit 1
fi

# Generate random alphanumeric text
< /dev/urandom tr -dc 'A-Za-z0-9' | head -c "$BYTE_SIZE"