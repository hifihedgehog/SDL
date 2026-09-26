#!/usr/bin/env python3
#
# Prints the test vectors of test/testblezwiftcrypto.c as C arrays: the key
# exchange of older Zwift firmware, hifihedgehog/SDL#33 Part 12, Zwift replay
# test 14. The curve, ECDH, HKDF and AES-CCM here come from the cryptography
# package, which runs on OpenSSL and shares no code with the Windows CNG that
# src/joystick/ble/SDL_ble_zwift_crypto.c calls. Every value is fixed, so a
# second run prints the same block. Paste it over the generated block of the
# test.
#
# The exchange, from the part's section "Encrypted handshake on older
# firmware":
# - Each side has a P-256 key pair. A public key is X then Y, 32 bytes each,
#   big-endian, 64 bytes in all.
# - The shared secret is the X coordinate of the ECDH point, all 32 bytes.
# - HKDF with SHA-256 turns the secret into 36 bytes, salted with the
#   device's key then the host's key, with empty info. The AES-256 key is
#   bytes 0 to 31, the nonce prefix bytes 32 to 35.
# - A frame is 4 counter bytes, the AES-CCM ciphertext and a 4-byte tag. The
#   nonce is the prefix and the counter bytes, with no associated data.
#
# Usage: python zwift-crypto-vectors.py (cryptography 50.0.0 made the block
# in the test)

import hashlib

import cryptography
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

CURVE = ec.SECP256R1()

# SEC 2 v2, section 2.4.2 "Recommended Parameters secp256r1"
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFC
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551

# Zwift replay test 1, Makinolo's capture from a right half: A pressed
MESSAGE_PRESSED = bytes.fromhex("07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00")
# The same message with A released
MESSAGE_RELEASED = bytes.fromhex("07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00")

COUNTER_PRESSED = bytes([0x01, 0x00, 0x00, 0x00])
COUNTER_RELEASED = bytes([0x02, 0x00, 0x00, 0x00])


def fixed_scalar(label):
    """A private scalar from 1 to n - 1, from SHA-256 of a label"""
    return int.from_bytes(hashlib.sha256(label).digest(), "big") % (N - 1) + 1


def private_key(scalar):
    return ec.derive_private_key(scalar, CURVE)


def public_bytes(key):
    """X then Y, 32 bytes each, big-endian, as OpenSSL encodes them"""
    point = key.public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)
    assert len(point) == 65 and point[0] == 0x04
    return point[1:]


def on_curve(x, y):
    return x < P and y < P and (y * y - (x * x * x + A * x + B)) % P == 0


def accepted(key):
    """True when OpenSSL takes the 64 bytes as a public key"""
    try:
        ec.EllipticCurvePublicKey.from_encoded_point(CURVE, b"\x04" + key)
        return True
    except ValueError:
        return False


def shared_secret(key, peer):
    """ECDH: the X coordinate of the shared point, all 32 bytes"""
    public = ec.EllipticCurvePublicKey.from_encoded_point(CURVE, b"\x04" + peer)
    secret = key.exchange(ec.ECDH(), public)
    assert len(secret) == 32
    return secret


def hkdf(secret, salt):
    assert len(secret) == 32 and len(salt) == 128
    return HKDF(algorithm=hashes.SHA256(), length=36, salt=salt, info=b"").derive(secret)


def frame(output, counter, message):
    """4 counter bytes, the ciphertext and a 4-byte tag"""
    nonce = output[32:36] + counter
    sealed = AESCCM(output[:32], tag_length=4).encrypt(nonce, message, None)
    assert AESCCM(output[:32], tag_length=4).decrypt(nonce, sealed, None) == message
    return counter + sealed


class Exchange:
    """Both sides of one exchange, from the host's and the device's view"""

    def __init__(self, host, device):
        self.host_key = public_bytes(host)
        self.device_key = public_bytes(device)
        self.secret = shared_secret(host, self.device_key)
        assert self.secret == shared_secret(device, self.host_key)
        # The salt is the device's key then the host's key
        self.output = hkdf(self.secret, self.device_key + self.host_key)
        # The device's own SDL_ZwiftKeys puts its peer, the host, first
        self.mirror = hkdf(self.secret, self.host_key + self.device_key)
        self.pressed = frame(self.output, COUNTER_PRESSED, MESSAGE_PRESSED)
        self.released = frame(self.output, COUNTER_RELEASED, MESSAGE_RELEASED)


def first_scalar(test):
    """The first private scalar d = 1, 2, 3 and so on that passes the test"""
    scalar = 1
    while not test(private_key(scalar)):
        scalar += 1
    return scalar


def c_array(name, data, comment):
    lines = ["/* %s */" % comment, "static const uint8_t %s[%d] = {" % (name, len(data))]
    for start in range(0, len(data), 12):
        chunk = data[start:start + 12]
        lines.append("    " + ", ".join("0x%02X" % byte for byte in chunk) + ",")
    lines[-1] = lines[-1][:-1]
    lines.append("};")
    return "\n".join(lines)


def main():
    host_scalar = fixed_scalar(b"SDL Zwift test host key")
    device_scalar = fixed_scalar(b"SDL Zwift test device key")
    host = private_key(host_scalar)
    device = private_key(device_scalar)
    main_exchange = Exchange(host, device)

    # A host key whose X starts with a zero byte
    zero_x_scalar = first_scalar(lambda key: public_bytes(key)[0] == 0x00)
    zero_x_exchange = Exchange(private_key(zero_x_scalar), device)
    assert zero_x_exchange.host_key[0] == 0x00

    # A device key whose secret with the fixed host key starts with a zero byte
    zero_secret_scalar = first_scalar(lambda key: shared_secret(host, public_bytes(key))[0] == 0x00)
    zero_secret_exchange = Exchange(host, private_key(zero_secret_scalar))
    assert zero_secret_exchange.secret[0] == 0x00

    # The device's key with Y one higher, a point off the curve
    x = int.from_bytes(main_exchange.device_key[:32], "big")
    y = int.from_bytes(main_exchange.device_key[32:], "big")
    off_curve = x.to_bytes(32, "big") + ((y + 1) % P).to_bytes(32, "big")
    assert on_curve(x, y) and not on_curve(x, (y + 1) % P) and not accepted(off_curve)

    # The point with X = 0, which is on the curve, and the same point with p
    # added to X, which is out of range
    low_y = pow(B, (P + 1) // 4, P)
    assert low_y * low_y % P == B and on_curve(0, low_y)
    low_x = bytes(32) + low_y.to_bytes(32, "big")
    wide_x = P.to_bytes(32, "big") + low_y.to_bytes(32, "big")
    assert accepted(low_x) and not accepted(wide_x)

    blocks = [
        "/* Generated by test/ble/zwift-crypto-vectors.py with cryptography %s. Do not edit. */"
        % cryptography.__version__,
        c_array("message_pressed", MESSAGE_PRESSED, "Zwift test 1: a right half with A pressed"),
        c_array("message_released", MESSAGE_RELEASED, "The same message with A released"),
        c_array("host_d", host_scalar.to_bytes(32, "big"),
                "The host's private key, SHA-256 of \"SDL Zwift test host key\" reduced to 1..n-1"),
        c_array("host_key", main_exchange.host_key, "The host's public key, X then Y"),
        c_array("device_d", device_scalar.to_bytes(32, "big"),
                "The device's private key, SHA-256 of \"SDL Zwift test device key\" reduced to 1..n-1"),
        c_array("device_key", main_exchange.device_key, "The device's public key, X then Y"),
        c_array("session_hkdf", main_exchange.output, "HKDF output, salted with the device's key then the host's"),
        c_array("session_key", main_exchange.output[:32], "The AES-256 key, HKDF output bytes 0 to 31"),
        c_array("session_prefix", main_exchange.output[32:36], "The nonce prefix, HKDF output bytes 32 to 35"),
        c_array("mirror_hkdf", main_exchange.mirror, "HKDF output salted the other way round, the device's view"),
        c_array("frame_pressed", main_exchange.pressed, "message_pressed under counter 01 00 00 00"),
        c_array("frame_released", main_exchange.released, "message_released under counter 02 00 00 00"),
        c_array("zero_x_d", zero_x_scalar.to_bytes(32, "big"),
                "The first host private key d = 1, 2, 3 and so on whose X starts with 00, d = %d" % zero_x_scalar),
        c_array("zero_x_key", zero_x_exchange.host_key, "Its public key, X then Y"),
        c_array("zero_x_hkdf", zero_x_exchange.output, "HKDF output of that host key with the device's key"),
        c_array("zero_x_mirror", zero_x_exchange.mirror, "The same salted the other way round"),
        c_array("zero_x_pressed", zero_x_exchange.pressed, "message_pressed under counter 01 00 00 00"),
        c_array("zero_x_released", zero_x_exchange.released, "message_released under counter 02 00 00 00"),
        c_array("zero_secret_key", zero_secret_exchange.device_key,
                "The first device key d = 1, 2, 3 and so on whose secret with the host's starts with 00, d = %d"
                % zero_secret_scalar),
        c_array("zero_secret_hkdf", zero_secret_exchange.output, "HKDF output of the host's key with that device key"),
        c_array("off_curve_key", off_curve, "The device's key with Y one higher, off the curve"),
        c_array("low_x_key", low_x, "The point with X = 0, on the curve"),
        c_array("wide_x_key", wide_x, "The same point with X = p, out of range"),
        "/* End of generated vectors */",
    ]
    print("\n\n".join(blocks))


if __name__ == "__main__":
    main()
