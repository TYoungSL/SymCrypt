//
// dh.c   DH functions
//
// Copyright (c) Microsoft Corporation. Licensed under the MIT license.
//
//

#include "precomp.h"

/*
Input validation in DH

Jack Lloyd pointed out that we do not have any validation of the public key in our SymCryptDhSecretAgreement
function. In particular, he suggested verifying that Y is not 0, 1, or P-1. This seems a natural improvement,
but things get a little bit more complicated.

The primary purpose of SymCrypt is to be the core crypto library for Windows. The original Windows DH code dates
back to the 20th century, and SymCrypt has to be compatible. Adding a new check on Y, rejecting inputs that used
to work, is a big problem because something, somewhere, will break, and customers really don't like that.
To maintain backward compatibility we don't introduce breaking API changes unless we have a compelling reason.
So the question is: is there a compelling reason to veryfy that Y is not 0, 1, or P-1?

First, let's look at validation and authentication in a DH exchange. We'll use P for the group prime, G for
the generator, Q for the order of G mod P, Y for the public key, and x for the private key. The shared secret
S = Y^x mod P.

The group parameters (P, G, Q) need to be properly chosen. Malicous group parameters destroy the security of DH.
Proper validation is:
    - P is a prime of the right size (e.g. 2048 bits)
    - Q is a prime of the right size (e.g. 256 bits, or 2047 bits)
    - Q is a divisor of P-1
    - 2 <= G < P
    - G^Q mod P = 1
In some protocols the value of Q is not provided, which makes checking G much more complicated.

These validations are far too expensive to perform for every DH exchange. And in almost all protocols there is no
need to validate them. Some protocols use trusted group parameters that are part of the code. Other protocols have
one party authenticate the selected group parameters. (If a party authenticates bad group parameters then it is
malicous, and there is no point in trying to be secure when one of the parties involved is malicious.)
In practical terms, a protocol that uses DH with attacker-modifyable group parameters is simply insecure.

Now let's look at the public key Y. The recipient computes S := Y^x mod P. There are various unsuitable values that
the attacker can send instead of Y
    - Y = 0 leads to S = 0
    - Y = 1 leads to S = 1
    - Y = P-1 leads to S = 1 or P-1
    - a Y with small order modulo P leads to S being in a small set of known values
    - Y could be outside the subgroup generated by G. This is a breach of the protocol, but absent Y being in
        a small subgroup it is unclear whether this is a security issue.
If P is a 'safe' prime where Q = (P-1)/2 and Q is prime, there are no small subgroups apart from {1, P-1}.
However, many DH systems use DSA-like group structures for efficiency (the private key is smaller)
and those are not 'safe' primes so this only helps in some cases.

Let's see under what circumstances checking Y = 0, 1, or P-1 would help an application:
    - The group parameters are trusted or authenticated.
    - The group mod P does not have any small subgroups.
    - The protocol does not authenticate the public key Y
    - The protocol does authenticate S.
The last item is crucual. If S is not authenticated then an attacker can simply replace Y with its own G^z mod P
and use the private key z to recover S, so adding checks for Y in {0,1,P-1} would not fix the problem.

We are not aware of any of our products that uses DH in this way. The closest we can think of are some old secure
phones that would do a DH exchange and then authenticate S by having the parties verify a few digits of Hash(S) by
voice.

One imporant case to check is TLS which supports the DHE-RSA cipher suites.
In TLS the DHE_RSA cipher suite uses DH. The server's DH public key is authenticated by the server's signature.
Typically there is no client authentication. The client can't be fooled because of the server's signature, but
the attacker could set the client's DH public key and force the server to a known shared secret. But the attacker
could also just send a proper Y corresponding to its own private key and achieve the same effect, so the proposed
new checks don't actually help. Furthermore, without client authentication the attacker could just be the client.
If client authentication is used, the client signs the client's DH public key, so there is no problem at all.

Conclusion:
DH is hard to use right, and the protocol implementation has to consider many things. Y = 0, 1, or P-1 is just
one of many potential problems. Most protocol countermeasures against the other attacks also protect against the
Y = 0, 1, or P-1 issue. Absent a more concrete security problem with Y = 0, 1, or P-1 we do not see a
justification for making a backward-incompatible change at this layer of the code.

Niels, 20190704

 */

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptDhSecretAgreement(
    _In_                            PCSYMCRYPT_DLKEY        pkPrivate,
    _In_                            PCSYMCRYPT_DLKEY        pkPublic,
                                    SYMCRYPT_NUMBER_FORMAT  format,
                                    UINT32                  flags,
    _Out_writes_( cbAgreedSecret )  PBYTE                   pbAgreedSecret,
                                    SIZE_T                  cbAgreedSecret )
{
    SYMCRYPT_ERROR scError = SYMCRYPT_NO_ERROR;

    PBYTE   pbScratch = NULL;
    SIZE_T  cbScratch = 0;
    PBYTE   pbScratchInternal = NULL;
    SIZE_T  cbScratchInternal = 0;

    PCSYMCRYPT_DLGROUP  pDlgroup = NULL;

    PSYMCRYPT_MODELEMENT peRes = NULL;
    UINT32 cbModelement = 0;

    UINT32 nBitsOfExp = 0;

    // Make sure we only specify the correct flags and that
    // there is a private key
    if ( (flags != 0) || (!pkPrivate->fHasPrivateKey) )
    {
        scError = SYMCRYPT_INVALID_ARGUMENT;
        goto cleanup;
    }

    // Check that the group is the same for both keys
    if ( SymCryptDlgroupIsSame( pkPrivate->pDlgroup, pkPublic->pDlgroup ) )
    {
        pDlgroup = pkPrivate->pDlgroup;
    }
    else
    {
        scError = SYMCRYPT_INVALID_ARGUMENT;
        goto cleanup;
    }

    // Check the output buffer has the correct size
    if (cbAgreedSecret != SymCryptDlkeySizeofPublicKey( pkPrivate ))
    {
        scError = SYMCRYPT_WRONG_BLOCK_SIZE;
        goto cleanup;
    }

    // Objects and scratch space size calculation
    cbModelement = SymCryptSizeofModElementFromModulus( pDlgroup->pmP );
    cbScratch = cbModelement +
                SYMCRYPT_MAX( SYMCRYPT_SCRATCH_BYTES_FOR_MODEXP( pDlgroup->nDigitsOfP ),
                     SYMCRYPT_SCRATCH_BYTES_FOR_COMMON_MOD_OPERATIONS( pDlgroup->nDigitsOfP ));

    // Scratch space allocation
    pbScratch = SymCryptCallbackAlloc( cbScratch );
    if ( pbScratch == NULL )
    {
        scError = SYMCRYPT_MEMORY_ALLOCATION_FAILURE;
        goto cleanup;
    }

    // Creating temporary
    pbScratchInternal = pbScratch;
    cbScratchInternal = cbScratch;
    peRes = SymCryptModElementCreate( pbScratchInternal, cbModelement, pDlgroup->pmP );
    pbScratchInternal += cbModelement;
    cbScratchInternal -= cbModelement;

    SYMCRYPT_ASSERT( peRes != NULL);

    // Fix the bits of the exponent (the private key might be either mod Q, mod 2^nBitsPriv, or mod P)
    if (pkPrivate->fPrivateModQ)
    {
        nBitsOfExp = pkPrivate->nBitsPriv;
    }
    else
    {
        nBitsOfExp = pDlgroup->nBitsOfP;
    }

    // Calculate the secret
    SymCryptModExp(
            pDlgroup->pmP,
            pkPublic->pePublicKey,
            pkPrivate->piPrivateKey,
            nBitsOfExp,
            0,              // SC safe
            peRes,
            pbScratchInternal,
            cbScratchInternal );

    // Check if the result is zero
    if ( SymCryptModElementIsZero( pDlgroup->pmP, peRes ) )
    {
        scError = SYMCRYPT_INVALID_BLOB;
        goto cleanup;
    }

    // Output the result
    scError = SymCryptModElementGetValue(
            pDlgroup->pmP,
            peRes,
            pbAgreedSecret,
            cbAgreedSecret,
            format,
            pbScratchInternal,
            cbScratchInternal );
    if ( scError != SYMCRYPT_NO_ERROR )
    {
        goto cleanup;
    }

cleanup:
    if ( pbScratch != NULL )
    {
        SymCryptWipe( pbScratch, cbScratch );
        SymCryptCallbackFree( pbScratch );
    }

    return scError;
}
