#!/bin/bash
# Simple shell script server using netcat
# This is a minimal example for demonstration

PORT=8080

echo "Simple hook server listening on port $PORT"
echo "This is a basic example - use a proper server for production!"

while true; do
    # Listen for incoming connections
    (echo -ne "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"; \
     # Read the request
     REQUEST=$(timeout 5 nc -l $PORT | grep -A 1000 "^{" | head -n 1)
     
     # Simple parsing (very basic - just for demo)
     if echo "$REQUEST" | grep -q '"tool_name":"Bash"'; then
         if echo "$REQUEST" | grep -q 'rm -rf'; then
             echo '{"version":"1.0","decision":"block","reason":"Dangerous rm -rf command detected"}'
         else
             echo '{"version":"1.0","decision":"allow"}'
         fi
     else
         echo '{"version":"1.0","decision":"allow"}'
     fi
    ) | nc -l $PORT > /dev/null
done