/*
 * gcFuncs.c
 * Methods to implement gc-related activities of objects and classes
 *
 * Copyright (c) 1996, 1997, 1998, 1999, 2004
 *      Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file.
 */
/*
 * This file contains those functions that have to do with gc
 */

#include "config.h"
#include "debug.h"
#include "config-std.h"
#include "config-mem.h"
#include "gtypes.h"
#include "slots.h"
#include "access.h"
#include "object.h"
#include "errors.h"
#include "code.h"
#include "file.h"
#include "readClass.h"
#include "classMethod.h"
#include "baseClasses.h"
#include "stringSupport.h"
#include "thread.h"
#include "jthread.h"
#include "itypes.h"
#include "bytecode.h"
#include "exception.h"
#include "md.h"
#include "external.h"
#include "lookup.h"
#include "support.h"
#include "gc.h"
#include "locks.h"
#include "md.h"
#include "jni.h"
#include "soft.h"
#include "thread.h"
#include "methodCache.h"
#include "jvmpi_kaffe.h"

/*****************************************************************************
 * Class-related functions
 */

/*
 * Destroy a class object.
 */
static void
/* ARGSUSED */
destroyClass(Collector *collector, void* c)
{
        int i, j;
	int idx;
	Hjava_lang_Class* clazz = c;
	constants* pool;

DBG(CLASSGC,
        dprintf("destroying class %s @ %p\n",
		clazz->name ? clazz->name->data : "newborn", c);
   )
	assert(!CLASS_IS_PRIMITIVE(clazz)); 

	/* NB: Make sure that we don't unload fully loaded classes without
	 * classloaders.  This is wrong and indicate of a bug.
	 *
	 * NB: Note that this function must destroy any partially
	 * initialized class.  Class processing may fail at any
	 * state, and the discarded class objects destroyed.
	 */
	assert(clazz->state != CSTATE_COMPLETE || clazz->loader != 0);

#if defined(ENABLE_JVMPI)
	if( JVMPI_EVENT_ISENABLED(JVMPI_EVENT_CLASS_UNLOAD) )
	{
		JVMPI_Event ev;

		ev.event_type = JVMPI_EVENT_CLASS_UNLOAD;
		ev.u.class_unload.class_id = c;
		jvmpiPostEvent(&ev);
	}
#endif

	if (Kaffe_JavaVMArgs.enableVerboseGC > 0 && clazz->name) {
		dprintf("<GC: unloading class `%s'>\n",
			CLASS_CNAME(clazz));
	}

        /* destroy all fields */
        if (CLASS_FIELDS(clazz) != 0) {
                Field *f = CLASS_FIELDS(clazz);
                for (i = 0; i < CLASS_NFIELDS(clazz); i++) {
                        utf8ConstRelease(f->name);
                        /* if the field was never resolved, we must release the
                         * Utf8Const to which its type field points */
			utf8ConstRelease(f->signature);
			f++;
                }
                KFREE(CLASS_FIELDS(clazz));
        }

        /* destroy all methods, only if this class has indeed a method table */
        if (!CLASS_IS_ARRAY(clazz) && CLASS_METHODS(clazz) != 0) {
                Method *m = CLASS_METHODS(clazz);
                for (i = 0; i < CLASS_NMETHODS(clazz); i++) {
			void *ncode = 0;

			if (!CLASS_IS_INTERFACE(clazz))
			{
				ncode = METHOD_NATIVECODE(m);
#if defined(TRANSLATOR) && (defined (MD_UNREGISTER_JIT_EXCEPTION_INFO) || defined (JIT3))
				if (METHOD_JITTED(m)) {
#if defined(MD_UNREGISTER_JIT_EXCEPTION_INFO)
					MD_UNREGISTER_JIT_EXCEPTION_INFO (m->c.ncode.ncode_start,
									  ncode,
									  m->c.ncode.ncode_end);
#endif
#if defined(ENABLE_JVMPI)
					if( JVMPI_EVENT_ISENABLED(JVMPI_EVENT_COMPILED_METHOD_UNLOAD)  )
					{
						JVMPI_Event ev;

						ev.event_type = JVMPI_EVENT_COMPILED_METHOD_UNLOAD;
						ev.u.compiled_method_unload.
							method_id = m;
						jvmpiPostEvent(&ev);
					}
#endif
				}
#endif
			}
                        utf8ConstRelease(m->name);
                        utf8ConstRelease(METHOD_SIG(m));
                        KFREE(METHOD_PSIG(m));
                        KFREE(m->lines);
			KFREE(m->lvars);
			if( m->ndeclared_exceptions != -1 )
				KFREE(m->declared_exceptions);
                        KFREE(m->exception_table);
                        KFREE(m->c.bcode.code);	 /* aka c.ncode.ncode_start */

			/* Free ncode if necessary: this concerns
			 * any uninvoked trampolines
			 */
			if (KGC_getObjectIndex(collector, ncode) != -1) {
				KFREE(ncode);
			}
			m++;
                }
                KFREE(CLASS_METHODS(clazz));
        }

        /* release remaining refs to utf8consts in constant pool */
	pool = CLASS_CONSTANTS (clazz);
	for (idx = 0; idx < pool->size; idx++) {
		switch (pool->tags[idx]) {
		case CONSTANT_String:	/* unresolved strings */
		case CONSTANT_Utf8:
			utf8ConstRelease(WORD2UTF(pool->data[idx]));
			break;
		}
	}
	/* free constant pool */
	if (pool->data != 0) {
		KFREE(pool->data);
	}

        /* free various other fixed things */
        KFREE(CLASS_STATICDATA(clazz));
	if( clazz->vtable )
	{
		for( i = 0; i < clazz->msize; i++ )
		{
			if( clazz->vtable->method[i] == 0 )
				continue;
			/* Free ncode if necessary: this concerns
			 * any uninvoked trampolines
			 */
			if (KGC_getObjectIndex(collector,
					      clazz->vtable->method[i])
			    == KGC_ALLOC_DISPATCHTABLE) {
				KFREE(clazz->vtable->method[i]);
			}
		}
		KFREE(clazz->vtable);
	}
        KFREE(clazz->if2itable);
	if( clazz->itable2dtable )
	{
		for (i = 0; i < clazz->total_interface_len; i++) {
			Hjava_lang_Class* iface = clazz->interfaces[i];

			/* only if interface has not been freed already */
			if (KGC_getObjectIndex(collector, iface)
			    == KGC_ALLOC_CLASSOBJECT)
			{
				iface->implementors[clazz->impl_index] = -1;
			}
		}

		/* NB: we can't just sum up the msizes of the interfaces
		 * here because they might be destroyed simultaneously
		 */
		j = KGC_getObjectSize(collector, clazz->itable2dtable)
			/ sizeof (void*);
		for( i = 0; i < j; i++ )
		{
			if (KGC_getObjectIndex(collector,
					      clazz->itable2dtable[i])
			    == KGC_ALLOC_DISPATCHTABLE) {
				KGC_free(collector, clazz->itable2dtable[i]);
			}
		}
		KGC_free(collector, clazz->itable2dtable);
	}
	if( clazz->gc_layout &&
	    (clazz->superclass->gc_layout != clazz->gc_layout) )
	{
		KFREE(clazz->gc_layout);
	}
	KFREE(clazz->sourcefile);
	KFREE(clazz->implementors);
	KFREE(clazz->inner_classes);

        /* The interface table for array classes points to static memory */
        if (!CLASS_IS_ARRAY(clazz)) {
                KFREE(clazz->interfaces);
        }
        utf8ConstRelease(clazz->name);
}

/*
 * Walk the methods of a class.
 */
static
void
walkMethods(Collector* collector, void *gc_info, Method* m, int nm)
{
        while (nm-- > 0) {
                KGC_markObject(collector, gc_info, m->class);

                /* walk exception table in order to keep resolved catch types
                   alive */
                if (m->exception_table != 0) {
                        jexceptionEntry* eptr = &m->exception_table->entry[0];
                        int i;

                        for (i = 0; i < m->exception_table->length; i++) {
                                Hjava_lang_Class* c = eptr[i].catch_type;
                                if (c != 0 && c != UNRESOLVABLE_CATCHTYPE) {
                                        KGC_markObject(collector, gc_info, c);
                                }
                        }
                }
                m++;
        }
}

/*
 * Walk a class object.
 */
static void
walkClass(Collector* collector, void *gc_info, void* base, uint32 size UNUSED)
{
        Hjava_lang_Class* class;
        Field* fld;
        int n;
        constants* pool;
        int idx;

        class = (Hjava_lang_Class*)base;

DBG(GCPRECISE,
        dprintf("walkClass `%s' state=%d\n", CLASS_CNAME(class), class->state);
    )

        if (class->state >= CSTATE_PREPARED) {
                KGC_markObject(collector, gc_info, class->superclass);
        }

        /* walk constant pool - only resolved classes and strings count */
        pool = CLASS_CONSTANTS(class);
        for (idx = 0; idx < pool->size; idx++) {
                switch (pool->tags[idx]) {
                case CONSTANT_ResolvedClass:
			assert(!CLASS_IS_PRIMITIVE(CLASS_CLASS(idx, pool)));
                        KGC_markObject(collector, gc_info, CLASS_CLASS(idx, pool));
                        break;
                case CONSTANT_ResolvedString:
                        KGC_markObject(collector, gc_info, (void*)pool->data[idx]);
                        break;
                }
        }

        /*
         * NB: We suspect that walking the class pool should suffice if
         * we ensured that all classes referenced from this would show up
         * as a ResolvedClass entry in the pool.  However, this is not
         * currently the case: for instance, resolved field type are not
         * marked as resolved in the constant pool, even though they do
         * have an index there! XXX
         *
         * The second hypothesis is that if the class is loaded by the
         * system and thus anchored, then everything that we can reach from
         * here is anchored as well.  If that property holds, we should be
         * able to just return if class->loader == null here.   XXX
         */
        /* walk fields */
        if (CLASS_FIELDS(class) != 0) {

                /* walk all fields to keep their types alive */
                fld = CLASS_FIELDS(class);
                for (n = 0; n < CLASS_NFIELDS(class); n++) {
			/* don't mark field types that are primitive classes */
                        if (FIELD_RESOLVED(fld)
				&& !CLASS_IS_PRIMITIVE(fld->type))
			{
				if (!CLASS_GCJ(fld->type)) {
					KGC_markObject(collector, gc_info, fld->type);
				}
                        } /* else it's an Utf8Const that is not subject to gc */
                        fld++;
                }

                /* walk static fields that contain references */
                fld = CLASS_SFIELDS(class);
                for (n = 0; n < CLASS_NSFIELDS(class); n++) {
		    	/* Note that we must resolve field types eagerly
			 * in processClass for gcj code cause it may never
			 * call anything like getField to get the field
			 * type resolved.  This can happen for such types as [C
			 */
                        if (FIELD_RESOLVED(fld) && FIELD_ISREF(fld)) {
				void **faddr = (void**)FIELD_ADDRESS(fld);
#if defined (HAVE_GCJ_SUPPORT)
/* 1. GCJ work-around, see
 * http://sourceware.cygnus.com/ml/java-discuss/1999-q4/msg00379.html
 */
				if (FIELD_TYPE(fld) == StringClass) {
					KGC_markAddress(collector, gc_info, *faddr);
				} else {
					KGC_markObject(collector, gc_info, *faddr);
				}
#else
				KGC_markObject(collector, gc_info, *faddr);
#endif
                        }
                        fld++;
                }
        }

        /* The interface table for array classes points to static memory,
         * so we must not mark it.  */
        if (!CLASS_IS_ARRAY(class)) {
                /* mark interfaces referenced by this class */
                for (n = 0; n < class->total_interface_len; n++) {
                        KGC_markObject(collector, gc_info, class->interfaces[n]);
                }
        } else {
                /* array classes should keep their element type alive */
		Hjava_lang_Class *etype = CLASS_ELEMENT_TYPE(class);
		if (etype && !CLASS_IS_PRIMITIVE(etype)) {
			KGC_markObject(collector, gc_info, etype);
		}
        }

        /* CLASS_METHODS only points to the method array for non-array and
         * non-primitive classes */
        if (!CLASS_IS_PRIMITIVE(class) && !CLASS_IS_ARRAY(class) && CLASS_METHODS(class) != 0) {
                walkMethods(collector, gc_info, CLASS_METHODS(class), CLASS_NMETHODS(class));
        }
        KGC_markObject(collector, gc_info, class->loader);
}

/*****************************************************************************
 * various walk functions functions
 */
/*
 * Walk an array object objects.
 */
static
void
walkRefArray(Collector* collector, void *gc_info, void* base, uint32 size UNUSED)
{
        Hjava_lang_Object* arr;
        int i;
        Hjava_lang_Object** ptr;

        arr = (Hjava_lang_Object*)base;
        if (arr->vtable == 0) {                 /* see walkObject */
                return;
        }

        ptr = OBJARRAY_DATA(arr);
        /* mark class only if not a system class (which would be anchored
         * anyway.)  */
        if (arr->vtable->class->loader != 0) {
                KGC_markObject(collector, gc_info, arr->vtable->class);
        }

        for (i = ARRAY_SIZE(arr); --i>= 0; ) {
                Hjava_lang_Object* el = *ptr++;
		/*
		 * NB: This would break if some objects (i.e. class objects)
		 * are not gc-allocated.
		 */
		KGC_markObject(collector, gc_info, el);
        }
}

/*
 * Walk an object.
 */
static
void
walkObject(Collector* collector, void *gc_info, void* base, uint32 size)
{
        Hjava_lang_Object *obj = (Hjava_lang_Object*)base;
        Hjava_lang_Class *clazz;
        int *layout;
        int8* mem;
        int i, l, nbits;

        /*
         * Note that there is a window after the object is allocated but
         * before dtable is set.  In this case, we don't have to walk anything.
         */
        if (obj->vtable == 0)
                return;

        /* retrieve the layout of this object from its class */
        clazz = obj->vtable->class;

        /* class without a loader, i.e., system classes are anchored so don't
         * bother marking them.
         */
        if (clazz->loader != 0) {
                KGC_markObject(collector, gc_info, clazz);
        }

        layout = clazz->gc_layout;
        nbits = CLASS_FSIZE(clazz)/ALIGNMENTOF_VOIDP;

DBG(GCPRECISE,
        dprintf("walkObject `%s' ", CLASS_CNAME(clazz));
        BITMAP_DUMP(layout, nbits)
        dprintf(" (nbits=%d) %p-%p\n", nbits, base, ((char *)base) + size);
    )

        assert(CLASS_FSIZE(clazz) > 0);
        assert(size > 0);

        mem = (int8 *)base;

        /* optimize this! */
        while (nbits > 0) {
                /* get next integer from bitmap */
                l = *layout++;
                i = 0;
                while (i < BITMAP_BPI) {
                        /* skip the rest if no refs left */
                        if (l == 0) {
                                mem += (BITMAP_BPI - i) * ALIGNMENTOF_VOIDP;
                                break;
                        }

                        if (l < 0) {
                                /* we know this pointer points to gc'ed memory
                                 * there is no need to check - go ahead and
                                 * mark it.  Note that p may or may not point
                                 * to a "real" Java object.
                                 */
				void *p = *(void **)mem;
				KGC_markObject(collector, gc_info, p);
                        }
                        i++;
                        l <<= 1;
                        mem += ALIGNMENTOF_VOIDP;
                }
                nbits -= BITMAP_BPI;
        }
}

/*
 * Walk a loader object.
 */
static
void
walkLoader(Collector* collector, void *gc_info, void* base, uint32 size)
{
        walkObject(collector, gc_info, base, size);
        walkClassEntries(collector, gc_info, (Hjava_lang_ClassLoader*)base);
}

static
void
/* ARGSUSED */
finalizeObject(Collector* collector UNUSED, void* ob)
{
	JNIEnv *env = THREAD_JNIENV();
	Hjava_lang_Class* objclass;
        Hjava_lang_Object* obj = (Hjava_lang_Object*)ob;
	Method* final;

	if (!obj->vtable) {
		/* Suppose we catch ThreadDeath inside newObject() */
		return;
	}
        objclass = OBJECT_CLASS(obj);
        final = objclass->finalizer;

	if (!final) {
		assert(objclass->alloc_type == KGC_ALLOC_JAVALOADER);
		return;
	}

	(*env)->CallVoidMethod(env, obj, final);
	/* ignore any resulting exception */
	(*env)->ExceptionClear(env);
}

/*
 * Print a description of an object at a given address.
 * Single-threaded.
 */
char*
describeObject(const void* mem)
{
	static char buf[256];		/* BIG XXX */
	Hjava_lang_Class* clazz;
	Hjava_lang_String* str;
	Hjava_lang_Object* obj;
	char* c;
	jchar* jc;
	int l;

	int idx = KGC_getObjectIndex(main_collector, mem);
	switch (idx) {
	case KGC_ALLOC_JAVASTRING:

		str = (Hjava_lang_String*)mem;
		strcpy(buf, "java.lang.String `");
		c = buf + strlen(buf);
		jc = unhand(str)->value ? STRING_DATA(str) : 0;
		l = STRING_SIZE(str);
		while (jc && l-- > 0 && c < buf + sizeof(buf) - 2) {
			*c++ = (char)*jc++;
		}
		*c++ = '\'';
		*c = 0;
		break;

	case KGC_ALLOC_CLASSOBJECT:
		clazz = (Hjava_lang_Class*)mem;
		sprintf(buf, "java.lang.Class `%s'", clazz->name ?
			CLASS_CNAME(clazz) : "name unknown");
		break;

	case KGC_ALLOC_JAVALOADER:
	case KGC_ALLOC_NORMALOBJECT:
	case KGC_ALLOC_FINALIZEOBJECT:
	case KGC_ALLOC_REFARRAY:
	case KGC_ALLOC_PRIMARRAY:
		obj = (Hjava_lang_Object*)mem;
		if (obj->vtable != 0) {
			clazz = obj->vtable->class;
			sprintf(buf, "%s", CLASS_CNAME(clazz));
		} else {
			sprintf(buf, "newly born %s",
				KGC_getObjectDescription(main_collector, mem));
		}
		break;

	/* add more? */

	default:
		return ((char*)KGC_getObjectDescription(main_collector, mem));
	}
	return (buf);
}

Collector*
initCollector(void)
{
	Collector *gc = createGC();

	DBG(INIT, dprintf("initCollector()\n"); )

	KGC_registerGcTypeByIndex(gc, KGC_ALLOC_JAVASTRING,
	    stringWalk, KGC_OBJECT_NORMAL, stringDestroy, "j.l.String");
	KGC_registerGcTypeByIndex(gc, KGC_ALLOC_NOWALK,
	    0, KGC_OBJECT_NORMAL, 0, "other-nowalk");
	KGC_registerGcTypeByIndex(gc, KGC_ALLOC_NORMALOBJECT,
	    walkObject, KGC_OBJECT_NORMAL, 0, "obj-no-final");
	KGC_registerGcTypeByIndex(gc, KGC_ALLOC_PRIMARRAY,
	    0, KGC_OBJECT_NORMAL, 0, "prim-arrays");
	KGC_registerGcTypeByIndex(gc, KGC_ALLOC_REFARRAY,
	    walkRefArray, KGC_OBJECT_NORMAL, 0, "ref-arrays");
	KGC_registerGcTypeByIndex(gc, KGC_ALLOC_CLASSOBJECT,
	    walkClass, KGC_OBJECT_NORMAL, destroyClass, "j.l.Class");
	KGC_registerGcTypeByIndex(gc, KGC_ALLOC_FINALIZEOBJECT,
	    walkObject, finalizeObject, 0, "obj-final");
	KGC_registerGcTypeByIndex(gc, KGC_ALLOC_JAVALOADER,
	    walkLoader, finalizeObject, destroyClassLoader,
	    "j.l.ClassLoader");

	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_BYTECODE, "java-bytecode");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_EXCEPTIONTABLE, "exc-table");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JITCODE, "jitcode");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_STATICDATA, "static-data");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_CONSTANT, "constants");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_FIXED, "other-fixed");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_DISPATCHTABLE, "dtable");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_METHOD, "methods");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_FIELD, "fields");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_UTF8CONST, "utf8consts");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_INTERFACE, "interfaces");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_LOCK, "locks");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_THREADCTX, "thread-ctxts");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_REF, "gc-refs");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JITTEMP, "jit-temp-data");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JAR, "jar");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_CODEANALYSE, "code-analyse");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_CLASSPOOL, "class-pool");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_LINENRTABLE, "linenr-table");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_LOCALVARTABLE, "lvar-table");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_DECLAREDEXC, "declared-exc");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_CLASSMISC, "class-misc");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_VERIFIER, "verifier");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_TRAMPOLINE, "trampoline");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_NATIVELIB, "native-lib");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JIT_SEQ, "jit-seq");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JIT_CONST, "jit-const");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JIT_ARGS, "jit-args");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JIT_FAKE_CALL, "jit-fake-call");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JIT_SLOTS, "jit-slots");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JIT_CODEBLOCK, "jit-codeblock");
	KGC_registerFixedTypeByIndex(gc, KGC_ALLOC_JIT_LABELS, "jit-labels");

	DBG(INIT, dprintf("initCollector() done\n"); )
	return (gc);
}
