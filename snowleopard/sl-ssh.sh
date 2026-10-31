#!/bin/bash
# ssh into the Snow Leopard guest (old OpenSSH 5.2: needs legacy algorithms);
# SSHPORT as given to sl-vm.sh (default 2222), SL_USER the guest account
# (default test), SL_KEY the private key (default work/sl_key)
exec ssh -i "${SL_KEY:-$(dirname "$0")/work/sl_key}" -p "${SSHPORT:-2222}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  -o KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 \
  -o Ciphers=+aes128-cbc,3des-cbc -o MACs=+hmac-sha1 -o LogLevel=ERROR "${SL_USER:-test}@127.0.0.1" "$@"
