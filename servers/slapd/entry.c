/* entry.c - routines for dealing with entries */
/* $OpenLDAP$ */
/*
 * Copyright 1998-2000 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/ctype.h>
#include <ac/errno.h>
#include <ac/socket.h>
#include <ac/string.h>

#include "slap.h"
#include "ldif.h"

static unsigned char	*ebuf;	/* buf returned by entry2str		 */
static unsigned char	*ecur;	/* pointer to end of currently used ebuf */
static int		emaxsize;/* max size of ebuf			 */

/*
 * Empty root entry
 */
const Entry slap_entry_root = { NOID, "", "", NULL, NULL };

int entry_destroy(void)
{
	free( ebuf );
	ebuf = NULL;
	ecur = NULL;
	emaxsize = 0;
	return 0;
}


Entry *
str2entry( char *s )
{
	int rc;
	Entry		*e;
	char		*type;
	struct berval value;
	struct berval	*vals[2];
	AttributeDescription *ad;
	const char *text;
	char	*next;

	/*
	 * LDIF is used as the string format.
	 * An entry looks like this:
	 *
	 *	dn: <dn>\n
	 *	[<attr>:[:] <value>\n]
	 *	[<tab><continuedvalue>\n]*
	 *	...
	 *
	 * If a double colon is used after a type, it means the
	 * following value is encoded as a base 64 string.  This
	 * happens if the value contains a non-printing character
	 * or newline.
	 */

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL1,
		   "str2entry: \"%s\"\n", s ? s : "NULL" ));
#else
	Debug( LDAP_DEBUG_TRACE, "=> str2entry\n",
		s ? s : "NULL", 0, 0 );
#endif

	/* initialize reader/writer lock */
	e = (Entry *) ch_malloc( sizeof(Entry) );

	if( e == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "str2entry: entry allocation failed.\n" ));
#else
		Debug( LDAP_DEBUG_ANY,
		    "<= str2entry NULL (entry allocation failed)\n",
		    0, 0, 0 );
#endif
		return( NULL );
	}

	/* initialize entry */
	e->e_id = NOID;
	e->e_dn = NULL;
	e->e_ndn = NULL;
	e->e_attrs = NULL;
	e->e_private = NULL;

	/* dn + attributes */
	vals[0] = &value;
	vals[1] = NULL;

	next = s;
	while ( (s = ldif_getline( &next )) != NULL ) {
		if ( *s == '\n' || *s == '\0' ) {
			break;
		}

		if ( ldif_parse_line( s, &type, &value.bv_val, &value.bv_len ) != 0 ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL1,
				   "str2entry:  NULL (parse_line)\n" ));
#else
			Debug( LDAP_DEBUG_TRACE,
			    "<= str2entry NULL (parse_line)\n", 0, 0, 0 );
#endif
			continue;
		}

		if ( strcasecmp( type, "dn" ) == 0 ) {
			free( type );

			if ( e->e_dn != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL1, "str2entry: "
					"entry %ld has multiple dns \"%s\" and \"%s\" "
					"(second ignored)\n",
					(long) e->e_id, e->e_dn, value.bv_val != NULL ? value.bv_val : "" ));
#else
				Debug( LDAP_DEBUG_ANY, "str2entry: "
					"entry %ld has multiple dns \"%s\" and \"%s\" "
					"(second ignored)\n",
				    (long) e->e_id, e->e_dn,
					value.bv_val != NULL ? value.bv_val : "" );
#endif
				if( value.bv_val != NULL ) free( value.bv_val );
				continue;
			}

			e->e_dn = value.bv_val != NULL ? value.bv_val : ch_strdup( "" );
			continue;
		}

		ad = NULL;
		rc = slap_str2ad( type, &ad, &text );

		if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL1,
				   "str2entry:  str2ad(%s):	 %s\n", type, text ));
#else
			Debug( slapMode & SLAP_TOOL_MODE
				? LDAP_DEBUG_ANY : LDAP_DEBUG_TRACE,
				"<= str2entry: str2ad(%s): %s\n", type, text, 0 );
#endif
			if( slapMode & SLAP_TOOL_MODE ) {
				entry_free( e );
				free( value.bv_val );
				free( type );
				return NULL;
			}

			rc = slap_str2undef_ad( type, &ad, &text );

			if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL1,
					   "str2entry:  str2undef_ad(%s):  %s\n", type, text ));
#else
				Debug( LDAP_DEBUG_ANY,
					"<= str2entry: str2undef_ad(%s): %s\n",
						type, text, 0 );
#endif
				entry_free( e );
				free( value.bv_val );
				free( type );
				return NULL;
			}
		}

		if( slapMode & SLAP_TOOL_MODE ) {
			struct berval *pval;
			slap_syntax_validate_func *validate =
				ad->ad_type->sat_syntax->ssyn_validate;
			slap_syntax_transform_func *pretty =
				ad->ad_type->sat_syntax->ssyn_pretty;

			if( pretty ) {
				rc = pretty( ad->ad_type->sat_syntax,
					&value, &pval );

			} else if( validate ) {
				/*
			 	 * validate value per syntax
			 	 */
				rc = validate( ad->ad_type->sat_syntax, &value );

			} else {
#ifdef NEW_LOGGING
				LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
					   "str2entry: no validator for syntax %s\n", 
					   ad->ad_type->sat_syntax->ssyn_oid ));
#else
				Debug( LDAP_DEBUG_ANY,
					"str2entry: no validator for syntax %s\n",
					ad->ad_type->sat_syntax->ssyn_oid, 0, 0 );
#endif
				entry_free( e );
				free( value.bv_val );
				free( type );
				return NULL;
			}

			if( rc != 0 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
					   "str2entry:  invalid value for syntax %s\n",
					   ad->ad_type->sat_syntax->ssyn_oid ));
#else
				Debug( LDAP_DEBUG_ANY,
					"str2entry: invalid value for syntax %s\n",
					ad->ad_type->sat_syntax->ssyn_oid, 0, 0 );
#endif
				entry_free( e );
				free( value.bv_val );
				free( type );
				return NULL;
			}

			if( pretty ) {
				free( value.bv_val );
				value = *pval;
				free( pval );
			}
		}

		rc = attr_merge( e, ad, vals );

		if( rc != 0 ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL1,
				   "str2entry:  NULL (attr_merge)\n" ));
#else
			Debug( LDAP_DEBUG_ANY,
			    "<= str2entry NULL (attr_merge)\n", 0, 0, 0 );
#endif
			entry_free( e );
			free( value.bv_val );
			free( type );
			return( NULL );
		}

		free( type );
		free( value.bv_val );
	}

	/* check to make sure there was a dn: line */
	if ( e->e_dn == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
			   "str2entry:  entry %ld has no dn.\n",
				(long) e->e_id ));
#else
		Debug( LDAP_DEBUG_ANY, "str2entry: entry %ld has no dn\n",
		    (long) e->e_id, 0, 0 );
#endif
		entry_free( e );
		return( NULL );
	}

	/* generate normalized dn */
	e->e_ndn = ch_strdup( e->e_dn );
	(void) dn_normalize( e->e_ndn );

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL2,
		   "str2entry(%s) -> 0x%lx\n", e->e_dn, (unsigned long)e ));
#else
	Debug(LDAP_DEBUG_TRACE, "<= str2entry(%s) -> 0x%lx\n",
		e->e_dn, (unsigned long) e, 0 );
#endif
	return( e );
}


#define GRABSIZE	BUFSIZ

#define MAKE_SPACE( n )	{ \
		while ( ecur + (n) > ebuf + emaxsize ) { \
			ptrdiff_t	offset; \
			offset = (int) (ecur - ebuf); \
			ebuf = (unsigned char *) ch_realloc( (char *) ebuf, \
			    emaxsize + GRABSIZE ); \
			emaxsize += GRABSIZE; \
			ecur = ebuf + offset; \
		} \
	}

char *
entry2str(
    Entry	*e,
    int		*len )
{
	Attribute	*a;
	struct berval	*bv;
	int		i, tmplen;

	/*
	 * In string format, an entry looks like this:
	 *	dn: <dn>\n
	 *	[<attr>: <value>\n]*
	 */

	ecur = ebuf;

	/* put the dn */
	if ( e->e_dn != NULL ) {
		/* put "dn: <dn>" */
		tmplen = strlen( e->e_dn );
		MAKE_SPACE( LDIF_SIZE_NEEDED( 2, tmplen ));
		ldif_sput( (char **) &ecur, LDIF_PUT_VALUE, "dn", e->e_dn, tmplen );
	}

	/* put the attributes */
	for ( a = e->e_attrs; a != NULL; a = a->a_next ) {
		/* put "<type>:[:] <value>" line for each value */
		for ( i = 0; a->a_vals[i] != NULL; i++ ) {
			bv = a->a_vals[i];
			tmplen = a->a_desc->ad_cname.bv_len;
			MAKE_SPACE( LDIF_SIZE_NEEDED( tmplen, bv->bv_len ));
			ldif_sput( (char **) &ecur, LDIF_PUT_VALUE,
				a->a_desc->ad_cname.bv_val,
			    bv->bv_val, bv->bv_len );
		}
	}
	MAKE_SPACE( 1 );
	*ecur = '\0';
	*len = ecur - ebuf;

	return( (char *) ebuf );
}

void
entry_free( Entry *e )
{
	/* free an entry structure */
	assert( e != NULL );

	/* e_private must be freed by the caller */
	assert( e->e_private == NULL );
	e->e_private = NULL;

	/* free DNs */
	if ( e->e_dn != NULL ) {
		free( e->e_dn );
		e->e_dn = NULL;
	}
	if ( e->e_ndn != NULL ) {
		free( e->e_ndn );
		e->e_ndn = NULL;
	}

	/* free attributes */
	attrs_free( e->e_attrs );
	e->e_attrs = NULL;

	free( e );
}

/*
 * These routines are used only by Backend.
 *
 * the Entry has three entry points (ways to find things):
 *
 *	by entry	e.g., if you already have an entry from the cache
 *			and want to delete it. (really by entry ptr)
 *	by dn		e.g., when looking for the base object of a search
 *	by id		e.g., for search candidates
 *
 * these correspond to three different avl trees that are maintained.
 */

int
entry_cmp( Entry *e1, Entry *e2 )
{
	return( e1 < e2 ? -1 : (e1 > e2 ? 1 : 0) );
}

int
entry_dn_cmp( Entry *e1, Entry *e2 )
{
	/* compare their normalized UPPERCASED dn's */
	return( strcmp( e1->e_ndn, e2->e_ndn ) );
}

int
entry_id_cmp( Entry *e1, Entry *e2 )
{
	return( e1->e_id < e2->e_id ? -1 : (e1->e_id > e2->e_id ? 1 : 0) );
}

#ifdef SLAPD_BDB

/* This is like a ber_len */
static ber_len_t
entry_lenlen(ber_len_t len)
{
	if (len <= 0x7f)
		return 1;
	if (len <= 0xff)
		return 2;
	if (len <= 0xffff)
		return 3;
	if (len <= 0xffffff)
		return 4;
	return 5;
}

static void
entry_putlen(unsigned char **buf, ber_len_t len)
{
	ber_len_t lenlen = entry_lenlen(len);

	if (lenlen == 1) {
		**buf = (unsigned char) len;
	} else {
		int i;
		**buf = 0x80 | (lenlen - 1);
		for (i=lenlen-1; i>0; i--) {
			(*buf)[i] = (unsigned char) len;
			len >>= 8;
		}
	}
	*buf += lenlen;
}

static ber_len_t
entry_getlen(unsigned char **buf)
{
	ber_len_t len;
	int i;

	len = *(*buf)++;
	if (len <= 0x7f)
		return len;
	i = len & 0x7f;
	len = 0;
	for (;i > 0; i--) {
		len <<= 8;
		len |= *(*buf)++;
	}
	return len;
}

/* Flatten an Entry into a buffer. The buffer is filled with just the
 * strings/bervals of all the entry components. Each field is preceded
 * by its length, encoded the way ber_put_len works. Every field is NUL
 * terminated.  The entire buffer size is precomputed so that a single
 * malloc can be performed. The entry size is also recorded,
 * to aid in entry_decode.
 */
int entry_encode(Entry *e, struct berval *bv)
{
	int siz = sizeof(Entry);
	int len, dnlen, ndnlen;
	int i;
	Attribute *a;
	unsigned char *ptr;

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL1,
		"entry_encode: id: 0x%08lx  \"%s\"\n",
		(long) e->e_id, e->e_dn ));
#else
	Debug( LDAP_DEBUG_TRACE, "=> entry_encode(0x%08lx): %s\n",
		(long) e->e_id, e->e_dn, 0 );
#endif
	dnlen = strlen(e->e_dn);
	ndnlen = strlen(e->e_ndn);
	len = dnlen + ndnlen + 2;	/* two trailing NUL bytes */
	len += entry_lenlen(dnlen);
	len += entry_lenlen(ndnlen);
	for (a=e->e_attrs; a; a=a->a_next) {
		/* For AttributeDesc, we only store the attr name */
		siz += sizeof(Attribute);
		len += a->a_desc->ad_cname.bv_len+1;
		len += entry_lenlen(a->a_desc->ad_cname.bv_len);
		for (i=0; a->a_vals[i]; i++) {
			siz += sizeof(struct berval *);
			siz += sizeof(struct berval);
			len += a->a_vals[i]->bv_len + 1;
			len += entry_lenlen(a->a_vals[i]->bv_len);
		}
		len += entry_lenlen(i);
		siz += sizeof(struct berval *);	/* NULL pointer at end */
	}
	len += 1;	/* NUL byte at end */
	len += entry_lenlen(siz);
	bv->bv_len = len;
	bv->bv_val = ch_malloc(len);
	ptr = (unsigned char *)bv->bv_val;
	entry_putlen(&ptr, siz);
	entry_putlen(&ptr, dnlen);
	memcpy(ptr, e->e_dn, dnlen);
	ptr += dnlen;
	*ptr++ = '\0';
	entry_putlen(&ptr, ndnlen);
	memcpy(ptr, e->e_ndn, ndnlen);
	ptr += ndnlen;
	*ptr++ = '\0';

	for (a=e->e_attrs; a; a=a->a_next) {
		entry_putlen(&ptr, a->a_desc->ad_cname.bv_len);
		memcpy(ptr, a->a_desc->ad_cname.bv_val,
			a->a_desc->ad_cname.bv_len);
		ptr += a->a_desc->ad_cname.bv_len;
		*ptr++ = '\0';
		if (a->a_vals) {
		    for (i=0; a->a_vals[i]; i++);
		    entry_putlen(&ptr, i);
		    for (i=0; a->a_vals[i]; i++) {
			entry_putlen(&ptr, a->a_vals[i]->bv_len);
			memcpy(ptr, a->a_vals[i]->bv_val,
				a->a_vals[i]->bv_len);
			ptr += a->a_vals[i]->bv_len;
			*ptr++ = '\0';
		    }
		}
	}
	*ptr = '\0';
	return 0;
}

/* Retrieve an Entry that was stored using entry_encode above.
 * We malloc a single block with the size stored above for the Entry
 * and all if its Attributes. We also must lookup the stored
 * attribute names to get AttributeDescriptions. To detect if the
 * attributes of an Entry are later modified, we note that e->e_attr
 * is always a constant offset from (e).
 *
 * Note: everything is stored in a single contiguous block, so
 * you can not free individual attributes or names from this
 * structure. Attempting to do so will likely corrupt memory.
 */
int entry_decode(struct berval *bv, Entry **e)
{
	int i, j;
	int rc;
	Attribute *a;
	Entry *x;
	const char *text;
	AttributeDescription *ad;
	unsigned char *ptr = (unsigned char *)bv->bv_val;
	struct berval **bptr;
	struct berval *vptr;

	i = entry_getlen(&ptr);
	x = ch_malloc(i);
	i = entry_getlen(&ptr);
	x->e_dn = ptr;
	ptr += i+1;
	i = entry_getlen(&ptr);
	x->e_ndn = ptr;
	ptr += i+1;
#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL2,
		   "entry_decode: \"%s\"\n", x->e_dn ));
#else
	Debug( LDAP_DEBUG_TRACE,
	    "entry_decode: \"%s\"\n",
	    x->e_dn, 0, 0 );
#endif
	x->e_private = bv->bv_val;

	/* A valid entry must have at least one attr, so this
	 * pointer can never be NULL
	 */
	x->e_attrs = (Attribute *)(x+1);
	bptr = (struct berval **)x->e_attrs;
	a = NULL;

	while (i = entry_getlen(&ptr)) {
		if (a) {
			a->a_next = (Attribute *)bptr;
		}
		a = (Attribute *)bptr;
		ad = NULL;
		rc = slap_str2ad( ptr, &ad, &text );

		if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
				   "entry_decode: str2ad(%s): %s\n", ptr, text ));
#else
			Debug( LDAP_DEBUG_TRACE,
				"<= entry_decode: str2ad(%s): %s\n", ptr, text, 0 );
#endif
			rc = slap_str2undef_ad( ptr, &ad, &text );

			if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
					   "entry_decode:  str2undef_ad(%s): %s\n", ptr, text));
#else
				Debug( LDAP_DEBUG_ANY,
					"<= entry_decode: str2undef_ad(%s): %s\n",
						ptr, text, 0 );
#endif
				return rc;
			}
		}
		ptr += i + 1;
		a->a_desc = ad;
		bptr = (struct berval **)(a+1);
		a->a_vals = bptr;
		j = entry_getlen(&ptr);
		a->a_vals[j] = NULL;
		vptr = (struct berval *)(bptr + j + 1);

		while (j) {
			i = entry_getlen(&ptr);
			*bptr = vptr;
			vptr->bv_len = i;
			vptr->bv_val = (char *)ptr;
			ptr += i+1;
			bptr++;
			vptr++;
			j--;
		}
		bptr = (struct berval **)vptr;
	}
	if (a)
		a->a_next = NULL;
#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_DETAIL1,
		   "entry_decode:  %s\n", x->e_dn ));
#else
	Debug(LDAP_DEBUG_TRACE, "<= entry_decode(%s)\n",
		x->e_dn, 0, 0 );
#endif
	*e = x;
	return 0;
}
#endif
