/*
 * opencog/atoms/base/ClassServer.h
 *
 * Copyright (C) 2011 by The OpenCog Foundation
 * Copyright (C) 2017 by Linas Vepstas
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _OPENCOG_CLASS_SERVER_H
#define _OPENCOG_CLASS_SERVER_H

#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include <opencog/util/sigslot.h>
#include <opencog/atoms/proto/types.h>
#include <opencog/atoms/proto/atom_types.h>
#include <opencog/atoms/base/Handle.h>

namespace opencog
{
/** \addtogroup grp_atomspace
 *  @{
 */

typedef SigSlot<Type> TypeSignal;

/**
 * This class keeps track of the complete protoatom (value and atom)
 * class hierarchy. It also provides factories for those atom types
 * that have non-trivial C++ objects behind them.
 */
class ClassServer
{
public:
    // Currently, we provide factories only for atoms, not for
    // values. TruthValues could use a factory, but, for now,
    // we don't have a pressing reason to add that.
    typedef Handle (AtomFactory)(const Handle&);

    // Perform checking of the outgoing set, during construction.
    typedef bool (Validator)(const Handle&);
private:

    /** Private default constructor for this class to make it a singleton. */
    ClassServer();

    std::set< std::string > _loaded_modules;

    /* It is very tempting to make the type_mutex into a reader-writer
     * mutex. However, it appears that this is a bad idea: reader-writer
     * mutexes cause cache-line ping-ponging when there is contention,
     * effecitvely serializing access, and are just plain slower when
     * there is no contention.  Thus, the current implementations seem
     * to be a lose-lose proposition. See the Anthony Williams post here:
     * http://permalink.gmane.org/gmane.comp.lib.boost.devel/211180
     */
    mutable std::mutex type_mutex;

    Type nTypes;
    Type _maxDepth;

    std::vector< std::vector<bool> > inheritanceMap;
    std::vector< std::vector<bool> > recursiveMap;
    std::unordered_map<std::string, Type> name2CodeMap;
    std::vector<const std::string*> _code2NameMap;
    mutable std::vector<AtomFactory*> _atomFactory;
    mutable std::vector<Validator*> _validator;
    std::vector<int> _mod;
    TypeSignal _addTypeSignal;

    void setParentRecursively(Type parent, Type type, Type& maxd);

    void spliceFactory(Type, AtomFactory*);

public:
    /** Gets the singleton instance (following meyer's design pattern) */
    friend ClassServer& classserver();

    bool beginTypeDecls(const char *);
    void endTypeDecls(void);
    /**
     * Adds a new atom type with the given name and parent type.
     * Return a numeric value that is assigned to the new type.
     */
    Type declType(const Type parent, const std::string& name);

    /**
     * Declare a factory for an atom type.
     */
    void addFactory(Type, AtomFactory*);
    AtomFactory* getFactory(Type) const;

    /**
     * Declare a validator for an atom type.
     */
    void addValidator(Type, Validator*);
    Validator* getValidator(Type) const;

    /**
     * Convert the indicated Atom into a C++ instance of the
     * same type.
     */
    Handle factory(const Handle&) const;

    /** Provides ability to get type-added signals.
     * @warning methods connected to this signal must not call
     * ClassServer::addType or things will deadlock.
     */
    TypeSignal& typeAddedSignal();

    /**
     * Given the type `type`, get the immediate descendents.
     * This is NOT recursive, only the first generation is returned.
     * Stores the children types on the OutputIterator 'result'.
     * Returns the number of children types.
     */
    template<typename OutputIterator>
    unsigned long getChildren(Type type, OutputIterator result) const
    {
        unsigned long n_children = 0;
        for (Type i = 0; i < nTypes; ++i) {
            if (inheritanceMap[type][i] and (type != i)) {
                *(result++) = i;
                n_children++;
            }
        }
        return n_children;
    }

    /**
     * Given the type `type`, get the immediate parents.
     * This is NOT recursive, only the first generation is returned.
     * Stores the parent types on the OutputIterator 'result'.
     * Returns the number of parent types.
     */
    template<typename OutputIterator>
    unsigned long getParents(Type type, OutputIterator result) const
    {
        unsigned long n_parents = 0;
        for (Type i = 0; i < nTypes; ++i) {
            if (inheritanceMap[i][type] and (type != i)) {
                *(result++) = i;
                n_parents++;
            }
        }
        return n_parents;
    }

    /**
     * Given the type `type`, get all of the descendents.  This is
     * recursive, that is, children of the children are returned.
     * Stores the children types on the OutputIterator 'result'.
     * Returns the number of children types.
     */
    template <typename OutputIterator>
    unsigned long getChildrenRecursive(Type type, OutputIterator result) const
    {
        unsigned long n_children = 0;
        for (Type i = 0; i < nTypes; ++i) {
            if (recursiveMap[type][i] and (type != i)) {
                *(result++) = i;
                n_children++;
            }
        }
        return n_children;
    }

    template <typename Function>
    void foreachRecursive(Function func, Type type) const
    {
        for (Type i = 0; i < nTypes; ++i) {
            if (recursiveMap[type][i]) (func)(i);
        }
    }

    /**
     * Returns the total number of classes in the system.
     *
     * @return The total number of classes in the system.
     */
    Type getNumberOfClasses() const;

    /**
     * Returns whether a given class is assignable from another.
     * This is the single-most commonly called method in this class.
     *
     * @param super Super class.
     * @param sub Subclass.
     * @return Whether a given class is assignable from another.
     */
    bool isA(Type sub, Type super) const
    {
        /* Because this method is called extremely often, we want
         * the best-case fast-path for it.  Since updates are extremely
         * unlikely after initialization, we use a multi-reader lock,
         * and don't care at all about writer starvation, since there
         * will almost never be writers. However, see comments above
         * about multi-reader-locks -- we are not using them just right
         * now, because they don't seem to actually help.
         *
         * Currently, this lock accounts for 2% or 3% performance
         * impact on atom insertion into atomspace.  The unit tests
         * don't need it to pass.  Most users probably dont need it
         * at all, because most type creation/update happens in
         * shared-lib ctors, which mistly should be done by the time
         * that this gets called. How big a price do you want to pay
         * for avoiding a possible crash on a shared-lib load while
         * also running some multi-threaded app?
         */
        // std::lock_guard<std::mutex> l(type_mutex);
        if ((sub >= nTypes) || (super >= nTypes)) return false;
        return recursiveMap[super][sub];
    }

    bool isA_non_recursive(Type sub, Type super) const;

    /**
     * Returns true if given class is a Value.
     *
     * @param t class.
     * @return Whether a given class is Value.
     */
    bool isValue(Type t) const { return isA(t, VALUE); }

    /**
     * Returns true if given class is a valid atom type.
     *
     * @param t class.
     * @return Whether a given class is an atom.
     */
    bool isAtom(Type t) const { return isA(t, ATOM); }

    /**
     * Returns true if given class is a Node.
     *
     * @param t class.
     * @return Whether a given class is Node.
     */
    bool isNode(Type t) const { return isA(t, NODE); }

    /**
     * Returns true if given class is a Link.
     *
     * @param t class.
     * @return Whether a given class is Link.
     */
    bool isLink(Type t) const { return isA(t, LINK); }

    /**
     * Returns whether a class with name 'typeName' is defined.
     */
    bool isDefined(const std::string& typeName) const;

    /**
     * Returns the type of a given class.
     *
     * @param typeName Class type name.
     * @return The type of a givenn class.
     */
    Type getType(const std::string& typeName) const;

    /**
     * Returns the string representation of a given atom type.
     *
     * @param type Atom type code.
     * @return The string representation of a givenn class.
     */
    const std::string& getTypeName(Type type) const;
};

ClassServer& classserver();

#define DEFINE_LINK_FACTORY(CNAME,CTYPE)                          \
                                                                  \
Handle CNAME::factory(const Handle& base)                         \
{                                                                 \
   /* If it's castable, nothing to do. */                         \
   if (CNAME##Cast(base)) return base;                            \
                                                                  \
   /* Look to see if we have static typechecking to do */         \
   ClassServer::Validator* checker =                              \
       classserver().getValidator(base->get_type());              \
                                                                  \
   /* Well, is it OK, or not? */                                  \
   if (checker and not checker(base))                             \
       throw SyntaxException(TRACE_INFO,                          \
           "Invalid Atom syntax: %s", base->to_string().c_str()); \
                                                                  \
   Handle h(create##CNAME(base->getOutgoingSet(), base->get_type())); \
   return h;                                                      \
}                                                                 \
                                                                  \
/* This runs when the shared lib is loaded. */                    \
static __attribute__ ((constructor)) void init(void)              \
{                                                                 \
   classserver().addFactory(CTYPE, &CNAME::factory);              \
}

#define DEFINE_NODE_FACTORY(CNAME,CTYPE)                          \
                                                                  \
Handle CNAME::factory(const Handle& base)                         \
{                                                                 \
   if (CNAME##Cast(base)) return base;                            \
   Handle h(create##CNAME(base->get_type(), base->get_name()));   \
   return h;                                                      \
}                                                                 \
                                                                  \
/* This runs when the shared lib is loaded. */                    \
static __attribute__ ((constructor)) void init(void)              \
{                                                                 \
   classserver().addFactory(CTYPE, &CNAME::factory);              \
}

/** @}*/
} // namespace opencog

#endif // _OPENCOG_CLASS_SERVER_H
