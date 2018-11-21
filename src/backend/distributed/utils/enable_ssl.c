/*-------------------------------------------------------------------------
 *
 * enable_ssl.c
 *    UDF and Utilities for enabling ssl during citus setup
 *
 * Copyright (c) 2018, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "distributed/connection_management.h"
#include "distributed/worker_protocol.h"
#include "fmgr.h"
#include "libpq/libpq.h"
#include "nodes/parsenodes.h"
#include "postmaster/postmaster.h"
#include "utils/guc.h"

#ifdef USE_OPENSSL
#include "openssl/dsa.h"
#include "openssl/err.h"
#include "openssl/pem.h"
#include "openssl/rsa.h"
#include "openssl/ssl.h"
#include "openssl/x509.h"
#endif


#define DirectFunctionCall0(func) \
	DirectFunctionCall0Coll(func, InvalidOid)

#define ENABLE_SSL_QUERY "ALTER SYSTEM SET ssl TO on;"
#define RESET_CITUS_NODE_CONNINFO \
	"ALTER SYSTEM SET citus.node_conninfo TO 'sslmode=prefer';"

#define CITUS_AUTO_SSL_COMMON_NAME "citus-auto-ssl"
#define X509_SUBJECT_COMMON_NAME "CN"


/* forward declaration of helper functions */
static Datum DirectFunctionCall0Coll(PGFunction func, Oid collation);

/* use pg's implementation that is not exposed in a header file */
extern Datum pg_reload_conf(PG_FUNCTION_ARGS);


#ifdef USE_SSL

/* forward declaration of functions used when compiled with ssl */
static void EnsureReleaseOpenSSLResource(MemoryContextCallbackFunction free, void *arg);
static bool ShouldUseAutoSSL(void);
static bool CreateCertificatesWhenNeeded(void);
static EVP_PKEY * GeneratePrivateKey(void);
static X509 * CreateCertificate(EVP_PKEY *privateKey);
static bool StoreCertificate(EVP_PKEY *privateKey, X509 *certificate);
#endif /* USE_SSL */


PG_FUNCTION_INFO_V1(citus_setup_ssl);
PG_FUNCTION_INFO_V1(citus_reset_default_for_node_conninfo);


/*
 * citus_setup_ssl is called during the first creation of a citus extension. It configures
 * postgres to use ssl if not already on. During this process it will create certificates
 * if they are not already installed in the configured location.
 */
Datum
citus_setup_ssl(PG_FUNCTION_ARGS)
{
#ifndef USE_SSL
	ereport(WARNING, (errmsg("can not setup ssl on postgres that is not compiled with "
							 "ssl support")));
#else /* USE_SSL */
	if (!EnableSSL && ShouldUseAutoSSL())
	{
		Node *enableSSLParseTree = NULL;

		elog(LOG, "citus extension created on postgres without ssl enabled, turning it "
				  "on during creation of the extension");

		/* execute the alter system statement to enable ssl on within postgres */
		enableSSLParseTree = ParseTreeNode(ENABLE_SSL_QUERY);
		AlterSystemSetConfigFile((AlterSystemStmt *) enableSSLParseTree);

		/*
		 * ssl=on requires that a key and certificate are present, since we have
		 * enabled ssl mode here chances are the user didn't install credentials already.
		 *
		 * This function will check if they are available and if not it will generate a
		 * self singed certificate.
		 */
		CreateCertificatesWhenNeeded();

#if PG_VERSION_NUM >= 100000

		/*
		 * changing ssl configuration requires a reload of the configuration.
		 * To make sure the configuration is also loaded in the current postgres backend
		 * we also call the reload of the config file. This allows later checks during the
		 * CREATE/ALTER EXTENSION transaction to see the new values.
		 */
		DirectFunctionCall0(pg_reload_conf);
		ProcessConfigFile(PGC_SIGHUP);
#else /* PG_VERSION_NUM < 100000 */
		ereport(WARNING, (errmsg("restart of postgres required"),
						  errdetail("citus enables ssl in postgres. Postgres "
									"versions before 10.0 require a restart for changes "
									"to ssl to take effect."),
						  errhint("when restarting is not possible disable ssl and "
								  "change citus.conn_nodeinfo to have sslmode lower "
								  "then require.")));
#endif
	}
#endif /* USE_SSL */

	PG_RETURN_NULL();
}


/*
 * citus_reset_default_for_node_conninfo is called in the extension upgrade path when
 * users upgrade from a previous version to a version that has ssl enabled by default, and
 * only when the changed default value conflicts with the setup of the user.
 *
 * Once it is determined that the default value for citus.node_conninfo is used verbatim
 * with ssl not enabled on the cluster it will reinstate the old default value for
 * citus.node_conninfo.
 *
 * In effect this is to not impose the overhead of ssl on an already existing cluster that
 * didn't have it enabled already.
 */
Datum
citus_reset_default_for_node_conninfo(PG_FUNCTION_ARGS)
{
	Node *resetCitusNodeConnInfoParseTree = NULL;

	elog(LOG, "reset citus.node_conninfo to old default value as the new value is "
			  "incompatible with the current ssl setting");

	/* execute the alter system statement to reset node_conninfo to the old default*/
	resetCitusNodeConnInfoParseTree = ParseTreeNode(RESET_CITUS_NODE_CONNINFO);
	AlterSystemSetConfigFile((AlterSystemStmt *) resetCitusNodeConnInfoParseTree);

	/*
	 * changing citus.node_conninfo configuration requires a reload of the configuration.
	 * To make sure the configuration is also loaded in the current postgres backend
	 * we also call the reload of the config file. This allows later checks during the
	 * CREATE/ALTER EXTENSION transaction to see the new values.
	 */
	DirectFunctionCall0(pg_reload_conf);
	ProcessConfigFile(PGC_SIGHUP);

	PG_RETURN_NULL();
}


/*
 * DirectFunctionCall0Coll is based on the DirectFunctionCallNColl family of functions in
 * postgres, this time to call a function by its pointer without taking any parameters.
 */
static Datum
DirectFunctionCall0Coll(PGFunction func, Oid collation)
{
	FunctionCallInfoData fcinfo;
	Datum result;

	InitFunctionCallInfoData(fcinfo, NULL, 0, collation, NULL, NULL);

	result = (*func)(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
	{
		elog(ERROR, "function %p returned NULL", (void *) func);
	}

	return result;
}


#ifdef USE_SSL

/*
 * EnsureReleaseOpenSSLResource registers the openssl allocated resource to be freed when the
 * current memory context is reset.
 */
static void
EnsureReleaseOpenSSLResource(MemoryContextCallbackFunction free, void *arg)
{
	MemoryContextCallback *cb = MemoryContextAllocZero(CurrentMemoryContext,
													   sizeof(MemoryContextCallback));
	cb->func = free;
	cb->arg = arg;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, cb);
}


/*
 * ShouldUseAutoSSL checks if citus should enable ssl based on the connection settings it
 * uses for outward connections. When the outward connection is configured to require ssl
 * it assumes the other nodes in the network have the same setting and therefor it will
 * automatically enable ssl during installation.
 */
static bool
ShouldUseAutoSSL(void)
{
	const char *sslmode = NULL;
	sslmode = GetConnParam("sslmode");

	if (strcmp(sslmode, "require") == 0)
	{
		return true;
	}

	return false;
}


/*
 * CreateCertificatesWhenNeeded checks if the certificates exists. When they don't exist
 * they will be created. The return value tells whether or not new certificates have been
 * created. After this function it is guaranteed that certificates are in place. It is not
 * guaranteed they have the right permissions as we will not touch the keys if they exist.
 */
static bool
CreateCertificatesWhenNeeded()
{
	EVP_PKEY *privateKey = NULL;
	X509 *certificate = NULL;
	bool certificateWritten = false;
	SSL_CTX *sslContext = NULL;

	/*
	 * Since postgres might not have initialized ssl at this point we need to initialize
	 * it our self to be able to create a context. This code is less extensive then
	 * postgres' initialization but that will happen when postgres reloads its
	 * configuration with ssl enabled.
	 */
#ifdef HAVE_OPENSSL_INIT_SSL
	OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, NULL);
#else
	SSL_library_init();
#endif

	sslContext = SSL_CTX_new(SSLv23_method());
	if (!sslContext)
	{
		return false;
	}
	EnsureReleaseOpenSSLResource((MemoryContextCallbackFunction) & SSL_CTX_free,
								 sslContext);

	/*
	 * check if we can load the certificate, when we can we assume the certificates are in
	 * place. No need to create the certificates and we can exit the function
	 */
	if (SSL_CTX_use_certificate_chain_file(sslContext, ssl_cert_file) == 1)
	{
		return false;
	}
	elog(LOG, "no certificate present, generating self signed certificate");

	privateKey = GeneratePrivateKey();
	if (!privateKey)
	{
		ereport(ERROR, (errmsg("error while generating private key")));
		return false;
	}

	certificate = CreateCertificate(privateKey);
	if (!certificate)
	{
		ereport(ERROR, (errmsg("error while generating certificate")));
		return false;
	}

	certificateWritten = StoreCertificate(privateKey, certificate);
	if (!certificateWritten)
	{
		ereport(ERROR, (errmsg("error while storing key and certificate")));
		return false;
	}

	return true;
}


/*
 * GeneratePrivateKey uses open ssl functions to generate an RSA private key of 2048 bits.
 * All OpenSSL resources created during the process are added to the memory context active
 * when the function is called and therefore should not be freed by the caller.
 */
static EVP_PKEY *
GeneratePrivateKey()
{
	int ret = 0;
	EVP_PKEY *privateKey = NULL;
	BIGNUM *exponent = NULL;
	RSA *rsa = NULL;

	/* Allocate memory for the EVP_PKEY structure. */
	privateKey = EVP_PKEY_new();
	if (!privateKey)
	{
		ereport(ERROR, (errmsg("unable to allocate space for private key")));
		return NULL;
	}
	EnsureReleaseOpenSSLResource((MemoryContextCallbackFunction) & EVP_PKEY_free,
								 privateKey);

	exponent = BN_new();
	EnsureReleaseOpenSSLResource((MemoryContextCallbackFunction) & BN_free, exponent);

	ret = BN_set_word(exponent, RSA_F4);
	if (ret != 1)
	{
		ereport(ERROR, (errmsg("unable to prepare exponent for RSA algorithm")));
		return NULL;
	}

	rsa = RSA_new();
	ret = RSA_generate_key_ex(rsa, 2048, exponent, NULL);
	if (ret != 1)
	{
		ereport(ERROR, (errmsg("unable to generate RSA key")));
		return NULL;
	}

	if (!EVP_PKEY_assign_RSA(privateKey, rsa))
	{
		ereport(ERROR, (errmsg("unable to assign RSA key to use as private key")));
		return NULL;
	}

	/* The key has been generated, return it. */
	return privateKey;
}


/*
 * CreateCertificate creates a self signed certificate for citus to use. The certificate
 * will contain the public parts of the private key and will be signed in the end by the
 * private part to make it self signed.
 */
static X509 *
CreateCertificate(EVP_PKEY *privateKey)
{
	X509 *certificate = NULL;
	X509_NAME *subjectName = NULL;

	certificate = X509_new();
	if (!certificate)
	{
		ereport(ERROR, (errmsg("unable to allocate space for the x509 certificate")));
		return NULL;
	}
	EnsureReleaseOpenSSLResource((MemoryContextCallbackFunction) & X509_free,
								 certificate);

	/* Set the serial number. */
	ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);

	/*
	 * Set the expiry of the certificate.
	 *
	 * the functions X509_get_notBefore and X509_get_notAfter are deprecated, these are
	 * replaced with mutable and non-mutable variants in openssl 1.1, however they are
	 * better supported then the newer versions. In 1.1 they are aliasses to the mutable
	 * variant (X509_getm_notBefore, ...) that we actually need, so they will actually use
	 * the correct function in newer versions.
	 *
	 * Postgres does not check the validity on the certificates, but we can't omit the
	 * dates either to create a certificate that can be parsed.
	 * TODO settle on the actual validity times on the PR
	 */
	X509_gmtime_adj(X509_get_notBefore(certificate), 0);
	X509_gmtime_adj(X509_get_notAfter(certificate), 0);

	/* Set the public key for our certificate */
	X509_set_pubkey(certificate, privateKey);

	/* Set the common name for the certificate */
	subjectName = X509_get_subject_name(certificate);
	X509_NAME_add_entry_by_txt(subjectName, X509_SUBJECT_COMMON_NAME, MBSTRING_ASC,
							   (unsigned char *) CITUS_AUTO_SSL_COMMON_NAME, -1, -1,
							   0);

	/* For a self signed certificate we set the isser name to our own name */
	X509_set_issuer_name(certificate, subjectName);

	/* With all information filled out we sign the certificate with out own key */
	if (!X509_sign(certificate, privateKey, EVP_sha256()))
	{
		ereport(ERROR, (errmsg("unable to create signature for the x509 certificate")));
		return NULL;
	}

	return certificate;
}


/*
 * StoreCertificate stores both the private key and its certificate to the files
 * configured in postgres.
 */
static bool
StoreCertificate(EVP_PKEY *privateKey, X509 *certificate)
{
	const char *privateKeyFilename = ssl_key_file;
	const char *certificateFilename = ssl_cert_file;

	FILE *privateKeyFile = NULL;
	FILE *certificateFile = NULL;
	int success = 0;

	/* Open the private key file and write the private key in PEM format to it */
	privateKeyFile = fopen(privateKeyFilename, "wb");
	if (!privateKeyFile)
	{
		ereport(ERROR, (errmsg("unable to open private key file '%s' for writing",
							   privateKeyFilename)));
		return false;
	}

	success = PEM_write_PrivateKey(privateKeyFile, privateKey, NULL, NULL, 0, NULL, NULL);
	fclose(privateKeyFile);
	if (!success)
	{
		ereport(ERROR, (errmsg("unable to store private key")));
		return false;
	}

	/* Open the certificate file and write the certificate in the PEM format to it */
	certificateFile = fopen(certificateFilename, "wb");
	if (!certificateFile)
	{
		ereport(ERROR, (errmsg("unable to open certificate file '%s' for writing",
							   certificateFilename)));
		return false;
	}

	success = PEM_write_X509(certificateFile, certificate);
	fclose(certificateFile);
	if (!success)
	{
		ereport(ERROR, (errmsg("unable to store certificate")));
		return false;
	}

	return true;
}


#endif /* USE_SSL */
