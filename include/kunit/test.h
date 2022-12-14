/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Base unit test (KUnit) API.
 *
 * Copyright (C) 2018, Google LLC.
 * Author: Brendan Higgins <brendanhiggins@google.com>
 */

#ifndef _TEST_TEST_H
#define _TEST_TEST_H

#include <linux/types.h>
#include <linux/slab.h>
#include <kunit/strerror.h>
#include <kunit/test-stream.h>
#include <kunit/try-catch.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>

/**
 * struct test_resource - represents a *test managed resource*
 * @allocation: for the user to store arbitrary data.
 * @free: a user supplied function to free the resource. Populated by
 * test_alloc_resource().
 *
 * Represents a *test managed resource*, a resource which will automatically be
 * cleaned up at the end of a test case.
 *
 * Example:
 *
 * .. code-block:: c
 *
 *	struct test_kmalloc_params {
 *		size_t size;
 *		gfp_t gfp;
 *	};
 *
 *	static int test_kmalloc_init(struct test_resource *res, void *context)
 *	{
 *		struct test_kmalloc_params *params = context;
 *		res->allocation = kmalloc(params->size, params->gfp);
 *
 *		if (!res->allocation)
 *			return -ENOMEM;
 *
 *		return 0;
 *	}
 *
 *	static void test_kmalloc_free(struct test_resource *res)
 *	{
 *		kfree(res->allocation);
 *	}
 *
 *	void *test_kmalloc(struct test *test, size_t size, gfp_t gfp)
 *	{
 *		struct test_kmalloc_params params;
 *		struct test_resource *res;
 *
 *		params.size = size;
 *		params.gfp = gfp;
 *
 *		// TODO(felixguo@google.com): The & gets interpreted via
 *		// Kerneldoc but we don't want that.
 *		res = test_alloc_resource(test, test_kmalloc_init,
 *			test_kmalloc_free, & params);
 *		if (res)
 *			return res->allocation;
 *		else
 *			return NULL;
 *	}
 */
struct test_resource {
	void *allocation;
	void (*free)(struct test_resource *res);
	/* private: internal use only. */
	struct list_head node;
};

struct test;

/**
 * struct test_case - represents an individual test case.
 * @run_case: the function representing the actual test case.
 * @name: the name of the test case.
 *
 * A test case is a function with the signature, ``void (*)(struct test *)``
 * that makes expectations and assertions (see EXPECT_TRUE() and ASSERT_TRUE())
 * about code under test. Each test case is associated with a
 * &struct test_module and will be run after the module's init function and
 * followed by the module's exit function.
 *
 * A test case should be static and should only be created with the TEST_CASE()
 * macro; additionally, every array of test cases should be terminated with an
 * empty test case.
 *
 * Example:
 *
 * .. code-block:: c
 *
 *	void add_test_basic(struct test *test)
 *	{
 *		EXPECT_EQ(test, 1, add(1, 0));
 *		EXPECT_EQ(test, 2, add(1, 1));
 *		EXPECT_EQ(test, 0, add(-1, 1));
 *		EXPECT_EQ(test, INT_MAX, add(0, INT_MAX));
 *		EXPECT_EQ(test, -1, add(INT_MAX, INT_MIN));
 *	}
 *
 *	static struct test_case example_test_cases[] = {
 *		TEST_CASE(add_test_basic),
 *		{},
 *	};
 *
 */
struct test_case {
	void (*run_case)(struct test *test);
	const char name[256];
	/* private: internal use only. */
	bool success;
};

/**
 * TEST_CASE - A helper for creating a &struct test_case
 * @test_name: a reference to a test case function.
 *
 * Takes a symbol for a function representing a test case and creates a &struct
 * test_case object from it. See the documentation for &struct test_case for an
 * example on how to use it.
 */
#define TEST_CASE(test_name) { .run_case = test_name, .name = #test_name }

/**
 * struct test_module - describes a related collection of &struct test_case s.
 * @name: the name of the test. Purely informational.
 * @init: called before every test case.
 * @exit: called after every test case.
 * @test_cases: a null terminated array of test cases.
 *
 * A test_module is a collection of related &struct test_case s, such that
 * @init is called before every test case and @exit is called after every test
 * case, similar to the notion of a *test fixture* or a *test class* in other
 * unit testing frameworks like JUnit or Googletest.
 *
 * Every &struct test_case must be associated with a test_module for KUnit to
 * run it.
 */

#if !IS_ENABLED(CONFIG_KUNIT_TEST_WRAPPER_TEST)
struct test_module {
	const char name[256];
	int (*init)(struct test *test);
	void (*exit)(struct test *test);
	struct test_case *test_cases;
};
#else
struct test_module {
	const char name[256];
	char *log;
	int (*init)(struct test *test);
	void (*exit)(struct test *test);
	struct test_case *test_cases;
};
#endif /* CONFIG_KUNIT_TEST_WRAPPER_TEST */

struct test_initcall {
	struct list_head node;
	int (*init)(struct test_initcall *this, struct test *test);
	void (*exit)(struct test_initcall *this);
};

struct test_post_condition {
	struct list_head node;
	void (*validate)(struct test_post_condition *condition);
};

/**
 * struct test - represents a running instance of a test.
 * @priv: for user to store arbitrary data. Commonly used to pass data created
 * in the init function (see &struct test_module).
 *
 * Used to store information about the current context under which the test is
 * running. Most of this data is private and should only be accessed indirectly
 * via public functions; the one exception is @priv which can be used by the
 * test writer to store arbitrary data.
 */
struct test {
	void *priv;
	/* private: internal use only. */
	spinlock_t lock; /* Guards all mutable test state. */
	struct list_head resources;
	struct list_head post_conditions;
	const char *name;
	bool death_test;
	bool success;
	void (*vprintk)(const struct test *test,
			const char *level,
			struct va_format *vaf);
	void (*fail)(struct test *test, struct test_stream *stream);
	void (*abort)(struct test *test);
	struct test_try_catch try_catch;
};

static inline void test_set_death_test(struct test *test, bool death_test)
{
	unsigned long flags;

	spin_lock_irqsave(&test->lock, flags);
	test->death_test = death_test;
	spin_unlock_irqrestore(&test->lock, flags);
}

int test_init_test(struct test *test, const char *name);

int test_run_tests(struct test_module *module);

extern int register_kunit_notifier(struct notifier_block *nb);
extern int unregister_kunit_notifier(struct notifier_block *nb);

void test_install_initcall(struct test_initcall *initcall);

#define test_pure_initcall(fn) postcore_initcall(fn)

#define test_register_initcall(initcall) \
		static int register_test_initcall_##initcall(void) \
		{ \
			test_install_initcall(&initcall); \
			\
			return 0; \
		} \
		test_pure_initcall(register_test_initcall_##initcall)

#ifdef MODULE
#define kunit_test_suites_for_module(module)				\
	static int __init kunit_test_suites_init(void)			\
	{								\
		return 0;						\
	}								\
	module_init(kunit_test_suites_init);				\
									\
	static void __exit kunit_test_suites_exit(void)			\
	{								\
	}								\
	module_exit(kunit_test_suites_exit)
#else
#define kunit_test_suites_for_module(module)
#endif /* MODULE */

/**
 * module_test() - used to register a &struct test_module with KUnit.
 * @module: a statically allocated &struct test_module.
 *
 * Registers @module with the test framework. See &struct test_module for more
 * information.
 * Hardcoding the alignment to 8 was chosen as the most likely to remain
 * between the compiler laying out the test module pointers in the custom
 * section and the linker script placing the custom section in the output
 * binary. There must be no gap between the section start and the first
 * (test_module *) entry nor between any (test_module *) entries because
 * the test executor views the .test_modules section as an array of
 * (test_module *) starting at __test_modules_start.
 */
#define module_test(module) \
		kunit_test_suites_for_module(module); \
		static struct test_module *__test_module_##module __used       \
		__aligned(8) __attribute__((__section__(".test_modules"))) = \
			&module

#define module_test_for_module(module)	module_test(module)
/**
 * test_alloc_resource() - Allocates a *test managed resource*.
 * @test: The test context object.
 * @init: a user supplied function to initialize the resource.
 * @free: a user supplied function to free the resource.
 * @context: for the user to pass in arbitrary data.
 *
 * Allocates a *test managed resource*, a resource which will automatically be
 * cleaned up at the end of a test case. See &struct test_resource for an
 * example.
 */
struct test_resource *test_alloc_resource(struct test *test,
					  int (*init)(struct test_resource *,
						      void *),
					  void (*free)(struct test_resource *),
					  void *context);

void test_free_resource(struct test *test, struct test_resource *res);

/**
 * test_kmalloc() - Just like kmalloc() except the allocation is *test managed*.
 * @test: The test context object.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * Just like `kmalloc(...)`, except the allocation is managed by the test case
 * and is automatically cleaned up after the test case concludes. See &struct
 * test_resource for more information.
 */
void *test_kmalloc(struct test *test, size_t size, gfp_t gfp);

/**
 * test_kzalloc() - Just like test_kmalloc(), but zeroes the allocation.
 * @test: The test context object.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * See kzalloc() and test_kmalloc() for more information.
 */
static inline void *test_kzalloc(struct test *test, size_t size, gfp_t gfp)
{
	return test_kmalloc(test, size, gfp | __GFP_ZERO);
}

void test_cleanup(struct test *test);

void test_printk(const char *level,
		 const struct test *test,
		 const char *fmt, ...);

/**
 * test_info() - Prints an INFO level message associated with the current test.
 * @test: The test context object.
 * @fmt: A printk() style format string.
 *
 * Prints an info level message associated with the test module being run. Takes
 * a variable number of format parameters just like printk().
 */
#define test_info(test, fmt, ...) \
		test_printk(KERN_INFO, test, fmt, ##__VA_ARGS__)

/**
 * test_warn() - Prints a WARN level message associated with the current test.
 * @test: The test context object.
 * @fmt: A printk() style format string.
 *
 * See test_info().
 */
#define test_warn(test, fmt, ...) \
		test_printk(KERN_WARNING, test, fmt, ##__VA_ARGS__)

/**
 * test_err() - Prints an ERROR level message associated with the current test.
 * @test: The test context object.
 * @fmt: A printk() style format string.
 *
 * See test_info().
 */
#define test_err(test, fmt, ...) \
		test_printk(KERN_ERR, test, fmt, ##__VA_ARGS__)

static inline struct test_stream *test_expect_start(struct test *test,
						    const char *file,
						    const char *line)
{
	struct test_stream *stream = test_new_stream(test);

	stream->add(stream, "EXPECTATION FAILED at %s:%s\n\t", file, line);

	return stream;
}

static inline void test_expect_end(struct test *test,
				   bool success,
				   struct test_stream *stream)
{
	if (!success)
		test->fail(test, stream);
	else
		stream->clear(stream);
}

#define EXPECT_START(test) \
		test_expect_start(test, __FILE__, __stringify(__LINE__))

#define EXPECT_END(test, success, stream) test_expect_end(test, success, stream)

#define EXPECT(test, success, message) do {\
	struct test_stream *__stream = EXPECT_START(test); \
	\
	__stream->add(__stream, message); \
	EXPECT_END(test, success, __stream); \
} while (0)

/**
 * SUCCEED() - A no-op expectation. Only exists for code clarity.
 * @test: The test context object.
 *
 * The opposite of FAIL(), it is an expectation that cannot fail. In other
 * words, it does nothing and only exists for code clarity. See EXPECT_TRUE()
 * for more information.
 */
#define SUCCEED(test) do {} while (0)

/**
 * FAIL() - Always causes a test to fail when evaluated.
 * @test: The test context object.
 * @message: an informational message to be printed when the assertion is made.
 *
 * The opposite of SUCCEED(), it is an expectation that always fails. In other
 * words, it always results in a failed expectation, and consequently always
 * causes the test case to fail when evaluated. See EXPECT_TRUE() for more
 * information.
 */
#define FAIL(test, message) EXPECT(test, false, message)

/**
 * EXPECT_TRUE() - Causes a test failure when the given expression is not true.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression. The test fails when this does
 * not evaluate to true.
 *
 * This and expectations of the form `EXPECT_*` will cause the test case to fail
 * when the specified condition is not met; however, it will not prevent the
 * test case from continuing to run; this is otherwise known as an *expectation
 * failure*.
 */
#define EXPECT_TRUE(test, condition)					       \
		EXPECT(test, (condition),				       \
		       "Expected " #condition " is true, but is false.")

/**
 * EXPECT_FALSE() - Causes a test failure when the expression is not false.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression. The test fails when this does
 * not evaluate to false.
 *
 * Sets an expectation that @condition evaluates to false. See EXPECT_TRUE() for
 * more information.
 */
#define EXPECT_FALSE(test, condition)					       \
		EXPECT(test, !(condition),				       \
		       "Expected " #condition " is false, but is true.")

/**
 * EXPECT_NOT_NULL() - Causes a test failure when @expression is NULL.
 * @test: The test context object.
 * @expression: an arbitrary pointer expression. The test fails when this
 * evaluates to NULL.
 *
 * Sets an expectation that @expression does not evaluate to NULL. Similar to
 * EXPECT_TRUE() but supposed to be used with pointer expressions.
 */
#define EXPECT_NOT_NULL(test, expression)				       \
		EXPECT(test, (expression),				       \
		       "Expected " #expression " is not NULL, but is NULL.")

/**
 * EXPECT_NULL() - Causes a test failure when @expression is not NULL.
 * @test: The test context object.
 * @expression: an arbitrary pointer expression. The test fails when this does
 * not evaluate to NULL.
 *
 * Sets an expectation that @expression evaluates to NULL. Similar to
 * EXPECT_FALSE() but supposed to be used with pointer expressions.
 */
#define EXPECT_NULL(test, expression)					       \
		EXPECT(test, !(expression),				       \
		       "Expected " #expression " is NULL, but is not NULL.")

/**
 * EXPECT_SUCCESS() - Causes a test failure if @expression does not evaluate
 * to 0.
 * @test: The test context object.
 * @expression: an arbitrary expression evaluating to an int error code. The
 * test fails when this does not evaluate to 0.
 *
 * Sets an expectation that @expression evaluates to 0. Implementation assumes
 * that error codes are represented as negative values and if expression
 * evaluates to a negative value failure message will contain a mnemonic
 * representation of the error code (for example, for -1 it will contain EPERM).
 */
#define EXPECT_SUCCESS(test, expression) do {				       \
	struct test_stream *__stream = EXPECT_START(test);		       \
	typeof(expression) __result = (expression);			       \
	char buf[64];							       \
									       \
	if (__result != 0)						       \
		__stream->add(__stream,					       \
			      "Expected " #expression " is not error, "	       \
			      "but is: %s.",				       \
			      strerror_r(-__result, buf, sizeof(buf)));	       \
	EXPECT_END(test, __result == 0, __stream);			       \
} while (0)

/**
 * EXPECT_ERROR() - Causes a test failure when @expression does not evaluate to @errno.
 * @test: The test context object.
 * @expression: an arbitrary expression evaluating to an int error code. The
 * test fails when this does not evaluate to @errno.
 * @errno: expected error value, error values are expected to be negative.
 *
 * Sets an expectation that @expression evaluates to @errno, so as opposed to
 * EXPECT_SUCCESS it verifies that @expression evaluates to an error.
 */
#define EXPECT_ERROR(test, expression, errno) do {			       \
	struct test_stream *__stream = EXPECT_START(test);		       \
	typeof(expression) __result = (expression);			       \
	char buf1[64];							       \
	char buf2[64];							       \
									       \
	if (__result != errno)						       \
		__stream->add(__stream,					       \
			      "Expected " #expression " is %s, but is: %s.",   \
			      strerror_r(-errno, buf1, sizeof(buf1)),	       \
			      strerror_r(-__result, buf2, sizeof(buf2)));	       \
	EXPECT_END(test, __result == errno, __stream);			       \
} while (0)

static inline void test_expect_binary(struct test *test,
				      long long left, const char *left_name,
				      long long right, const char *right_name,
				      bool compare_result,
				      const char *compare_name,
				      const char *file,
				      const char *line)
{
	struct test_stream *stream = test_expect_start(test, file, line);

	stream->add(stream,
		    "Expected %s %s %s, but\n",
		    left_name, compare_name, right_name);
	stream->add(stream, "\t\t%s == %lld\n", left_name, left);
	stream->add(stream, "\t\t%s == %lld", right_name, right);

	test_expect_end(test, compare_result, stream);
}

/*
 * A factory macro for defining the expectations for the basic comparisons
 * defined for the built in types.
 *
 * Unfortunately, there is no common type that all types can be promoted to for
 * which all the binary operators behave the same way as for the actual types
 * (for example, there is no type that long long and unsigned long long can
 * both be cast to where the comparison result is preserved for all values). So
 * the best we can do is do the comparison in the original types and then coerce
 * everything to long long for printing; this way, the comparison behaves
 * correctly and the printed out value usually makes sense without
 * interpretation, but can always be interpretted to figure out the actual
 * value.
 */
#define EXPECT_BINARY(test, left, condition, right) do {		       \
	typeof(left) __left = (left);					       \
	typeof(right) __right = (right);				       \
	test_expect_binary(test,					       \
			   (long long) __left, #left,			       \
			   (long long) __right, #right,			       \
			   __left condition __right, #condition,	       \
			   __FILE__, __stringify(__LINE__));		       \
} while (0)

/**
 * EXPECT_EQ() - Sets an expectation that @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to EXPECT_TRUE(@test, (@left) ==
 * (@right)).  See EXPECT_TRUE() for more information.
 */
#define EXPECT_EQ(test, left, right) EXPECT_BINARY(test, left, ==, right)
#define EXPECT_PTR_EQ(test, left, right) EXPECT_BINARY(test, left, ==, right)

/**
 * EXPECT_NE() - An expectation that @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the values that @left and @right evaluate to are not
 * equal. This is semantically equivalent to EXPECT_TRUE(@test, (@left) !=
 * (@right)).  See EXPECT_TRUE() for more information.
 */
#define EXPECT_NE(test, left, right) EXPECT_BINARY(test, left, !=, right)
#define EXPECT_PTR_NE(test, left, right) EXPECT_BINARY(test, left, !=, right)

/**
 * EXPECT_LT() - An expectation that @left is less than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is less than the
 * value that @right evaluates to. This is semantically equivalent to
 * EXPECT_TRUE(@test, (@left) < (@right)). See EXPECT_TRUE() for more
 * information.
 */
#define EXPECT_LT(test, left, right) EXPECT_BINARY(test, left, <, right)

/**
 * EXPECT_LE() - An expectation that @left is less than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is less than or
 * equal to the value that @right evaluates to. Semantically this is equivalent
 * to EXPECT_TRUE(@test, (@left) <= (@right)). See EXPECT_TRUE() for more
 * information.
 */
#define EXPECT_LE(test, left, right) EXPECT_BINARY(test, left, <=, right)

/**
 * EXPECT_GT() - An expectation that @left is greater than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is greater than
 * the value that @right evaluates to. This is semantically equivalent to
 * EXPECT_TRUE(@test, (@left) > (@right)). See EXPECT_TRUE() for more
 * information.
 */
#define EXPECT_GT(test, left, right) EXPECT_BINARY(test, left, >, right)

/**
 * EXPECT_GE() - An expectation that @left is greater than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is greater than
 * the value that @right evaluates to. This is semantically equivalent to
 * EXPECT_TRUE(@test, (@left) >= (@right)). See EXPECT_TRUE() for more
 * information.
 */
#define EXPECT_GE(test, left, right) EXPECT_BINARY(test, left, >=, right)

/**
 * EXPECT_STREQ() - An expectation that strings @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * EXPECT_TRUE(@test, !strcmp((@left), (@right))). See EXPECT_TRUE() for more
 * information.
 */
#define EXPECT_STREQ(test, left, right) do {				       \
	struct test_stream *__stream = EXPECT_START(test);		       \
	typeof(left) __left = (left);					       \
	typeof(right) __right = (right);				       \
									       \
	__stream->add(__stream, "Expected " #left " == " #right ", but\n");    \
	__stream->add(__stream, "\t\t%s == %s\n", #left, __left);	       \
	__stream->add(__stream, "\t\t%s == %s\n", #right, __right);	       \
									       \
	EXPECT_END(test, !strcmp(left, right), __stream);		       \
} while (0)

/**
 * EXPECT_NOT_ERR_OR_NULL() - An expectation that @ptr is not null and not err.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an expectation that the value that @ptr evaluates to is not null and
 * not an errno stored in a pointer. This is semantically equivalent to
 * EXPECT_TRUE(@test, !IS_ERR_OR_NULL(@ptr)). See EXPECT_TRUE() for more
 * information.
 */
#define EXPECT_NOT_ERR_OR_NULL(test, ptr) do {				       \
	struct test_stream *__stream = EXPECT_START(test);		       \
	typeof(ptr) __ptr = (ptr);					       \
	char buf[64];							       \
									       \
	if (!__ptr)							       \
		__stream->add(__stream,					       \
			      "Expected " #ptr " is not null, but is.");       \
	if (IS_ERR(__ptr))						       \
		__stream->add(__stream,					       \
			      "Expected " #ptr " is not error, but is: %s",    \
			      strerror_r(-PTR_ERR(__ptr), buf, sizeof(buf)));  \
									       \
	EXPECT_END(test, !IS_ERR_OR_NULL(__ptr), __stream);		       \
} while (0)

static inline struct test_stream *test_assert_start(struct test *test,
						    const char *file,
						    const char *line)
{
	struct test_stream *stream = test_new_stream(test);

	stream->add(stream, "ASSERTION FAILED at %s:%s\n\t", file, line);

	return stream;
}

static inline void test_assert_end(struct test *test,
				   bool success,
				   struct test_stream *stream)
{
	if (!success) {
		test->fail(test, stream);
		test->abort(test);
	} else {
		stream->clear(stream);
	}
}

#define ASSERT_START(test) \
		test_assert_start(test, __FILE__, __stringify(__LINE__))

#define ASSERT_END(test, success, stream) test_assert_end(test, success, stream)

#define ASSERT(test, success, message) do {				       \
	struct test_stream *__stream = ASSERT_START(test);		       \
									       \
	__stream->add(__stream, message);				       \
	ASSERT_END(test, success, __stream);				       \
} while (0)

#define ASSERT_FAILURE(test, message) ASSERT(test, false, message)

/**
 * ASSERT_TRUE() - Causes an assertion failure when the expression is not true.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression. The test fails and aborts when
 * this does not evaluate to true.
 *
 * This and assertions of the form `ASSERT_*` will cause the test case to fail
 * *and immediately abort* when the specified condition is not met. Unlike an
 * expectation failure, it will prevent the test case from continuing to run;
 * this is otherwise known as an *assertion failure*.
 */
#define ASSERT_TRUE(test, condition)					       \
		ASSERT(test, (condition),				       \
		       "Asserted " #condition " is true, but is false.")

/**
 * ASSERT_FALSE() - Sets an assertion that @condition is false.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression.
 *
 * Sets an assertion that the value that @condition evaluates to is false.  This
 * is the same as EXPECT_FALSE(), except it causes an assertion failure (see
 * ASSERT_TRUE()) when the assertion is not met.
 */
#define ASSERT_FALSE(test, condition)					       \
		ASSERT(test, !(condition),				       \
		       "Asserted " #condition " is false, but is true.")

/**
 * ASSERT_NOT_NULL() - Asserts that @expression does not evaluate to NULL.
 * @test: The test context object.
 * @expression: an arbitrary pointer expression. The test fails when this
 * evaluates to NULL.
 *
 * Asserts that @expression does not evaluate to NULL, see EXPECT_NOT_NULL().
 */
#define ASSERT_NOT_NULL(test, expression)				       \
		ASSERT(test, (expression),				       \
		       "Expected " #expression " is not NULL, but is NULL.")

/**
 * ASSERT_NULL() - Asserts that @expression evaluates to NULL.
 * @test: The test context object.
 * @expression: an arbitrary pointer expression. The test fails when this does
 * not evaluate to NULL.
 *
 * Asserts that @expression evaluates to NULL, see EXPECT_NULL().
 */
#define ASSERT_NULL(test, expression)					       \
		ASSERT(test, !(expression),				       \
		       "Expected " #expression " is NULL, but is not NULL.")

/**
 * ASSERT_SUCCESS() - Asserts that @expression is 0.
 * @test: The test context object.
 * @expression: an arbitrary expression evaluating to an int error code.
 *
 * Asserts that @expression evaluates to 0. It's the same as EXPECT_SUCCESS.
 */
#define ASSERT_SUCCESS(test, expression) do {				       \
	struct test_stream *__stream = ASSERT_START(test);		       \
	typeof(expression) __result = (expression);			       \
	char buf[64];							       \
									       \
	if (__result != 0)						       \
		__stream->add(__stream,					       \
			      "Asserted " #expression " is not error, "	       \
			      "but is: %s.",				       \
			      strerror_r(-__result, buf, sizeof(buf)));	       \
	ASSERT_END(test, __result == 0, __stream);			       \
} while (0)

/**
 * ASSERT_ERROR() - Causes a test failure when @expression does not evaluate to
 * @errno.
 * @test: The test context object.
 * @expression: an arbitrary expression evaluating to an int error code. The
 * test fails when this does not evaluate to @errno.
 * @errno: expected error value, error values are expected to be negative.
 *
 * Asserts that @expression evaluates to @errno, similar to EXPECT_ERROR.
 */
#define ASSERT_ERROR(test, expression, errno) do {			       \
	struct test_stream *__stream = ASSERT_START(test);		       \
	typeof(expression) __result = (expression);			       \
	char buf1[64];							       \
	char buf2[64];							       \
									       \
	if (__result != errno)						       \
		__stream->add(__stream,					       \
			      "Expected " #expression " is %s, but is: %s.",   \
			      strerror_r(-errno, buf1, sizeof(buf1)),	       \
			      strerror_r(-__result, buf2, sizeof(buf2)));	       \
	ASSERT_END(test, __result == errno, __stream);			       \
} while (0)

static inline void test_assert_binary(struct test *test,
				      long long left, const char *left_name,
				      long long right, const char *right_name,
				      bool compare_result,
				      const char *compare_name,
				      const char *file,
				      const char *line)
{
	struct test_stream *stream = test_assert_start(test, file, line);

	stream->add(stream,
		    "Asserted %s %s %s, but\n",
		    left_name, compare_name, right_name);
	stream->add(stream, "\t\t%s == %lld\n", left_name, left);
	stream->add(stream, "\t\t%s == %lld", right_name, right);

	test_assert_end(test, compare_result, stream);
}

/*
 * A factory macro for defining the expectations for the basic comparisons
 * defined for the built in types.
 *
 * Unfortunately, there is no common type that all types can be promoted to for
 * which all the binary operators behave the same way as for the actual types
 * (for example, there is no type that long long and unsigned long long can
 * both be cast to where the comparison result is preserved for all values). So
 * the best we can do is do the comparison in the original types and then coerce
 * everything to long long for printing; this way, the comparison behaves
 * correctly and the printed out value usually makes sense without
 * interpretation, but can always be interpretted to figure out the actual
 * value.
 */
#define ASSERT_BINARY(test, left, condition, right) do {		       \
	typeof(left) __left = (left);					       \
	typeof(right) __right = (right);				       \
	test_assert_binary(test,					       \
			   (long long) __left, #left,			       \
			   (long long) __right, #right,			       \
			   __left condition __right, #condition,	       \
			   __FILE__, __stringify(__LINE__));		       \
} while (0)

/**
 * ASSERT_EQ() - Sets an assertion that @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal. This is the same as EXPECT_EQ(), except it causes an assertion failure
 * (see ASSERT_TRUE()) when the assertion is not met.
 */
#define ASSERT_EQ(test, left, right) ASSERT_BINARY(test, left, ==, right)
#define ASSERT_PTR_EQ(test, left, right) ASSERT_BINARY(test, left, ==, right)

/**
 * ASSERT_NE() - An assertion that @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the values that @left and @right evaluate to are not
 * equal. This is the same as EXPECT_NE(), except it causes an assertion failure
 * (see ASSERT_TRUE()) when the assertion is not met.
 */
#define ASSERT_NE(test, left, right) ASSERT_BINARY(test, left, !=, right)
#define ASSERT_PTR_NE(test, left, right) ASSERT_BINARY(test, left, !=, right)

/**
 * ASSERT_LT() - An assertion that @left is less than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is less than the
 * value that @right evaluates to.  This is the same as EXPECT_LT(), except it
 * causes an assertion failure (see ASSERT_TRUE()) when the assertion is not
 * met.
 */
#define ASSERT_LT(test, left, right) ASSERT_BINARY(test, left, <, right)

/**
 * ASSERT_LE() - An assertion that @left is less than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is less than or
 * equal to the value that @right evaluates to.  This is the same as
 * EXPECT_LE(), except it causes an assertion failure (see ASSERT_TRUE()) when
 * the assertion is not met.
 */
#define ASSERT_LE(test, left, right) ASSERT_BINARY(test, left, <=, right)

/**
 * ASSERT_GT() - An assertion that @left is greater than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is greater than the
 * value that @right evaluates to.  This is the same as EXPECT_GT(), except it
 * causes an assertion failure (see ASSERT_TRUE()) when the assertion is not
 * met.
 */
#define ASSERT_GT(test, left, right) ASSERT_BINARY(test, left, >, right)

/**
 * ASSERT_GE() - An assertion that @left is greater than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is greater than the
 * value that @right evaluates to.  This is the same as EXPECT_GE(), except it
 * causes an assertion failure (see ASSERT_TRUE()) when the assertion is not
 * met.
 */
#define ASSERT_GE(test, left, right) ASSERT_BINARY(test, left, >=, right)

/**
 * ASSERT_STREQ() - An assertion that strings @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal.  This is the same as EXPECT_STREQ(), except it causes an assertion
 * failure (see ASSERT_TRUE()) when the assertion is not met.
 */
#define ASSERT_STREQ(test, left, right) do {				       \
	struct test_stream *__stream = ASSERT_START(test);		       \
	typeof(left) __left = (left);					       \
	typeof(right) __right = (right);				       \
									       \
	__stream->add(__stream, "Asserted " #left " == " #right ", but\n");    \
	__stream->add(__stream, "\t\t%s == %s\n", #left, __left);	       \
	__stream->add(__stream, "\t\t%s == %s\n", #right, __right);	       \
									       \
	ASSERT_END(test, !strcmp(left, right), __stream);		       \
} while (0)

/**
 * ASSERT_NOT_ERR_OR_NULL() - An assertion that @ptr is not null and not err.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an assertion that the value that @ptr evaluates to is not null and not
 * an errno stored in a pointer.  This is the same as EXPECT_NOT_ERR_OR_NULL(),
 * except it causes an assertion failure (see ASSERT_TRUE()) when the assertion
 * is not met.
 */
#define ASSERT_NOT_ERR_OR_NULL(test, ptr) do {				       \
	struct test_stream *__stream = ASSERT_START(test);		       \
	typeof(ptr) __ptr = (ptr);					       \
	char buf[64];							       \
									       \
	if (!__ptr)							       \
		__stream->add(__stream,					       \
			      "Asserted " #ptr " is not null, but is.");       \
	if (IS_ERR(__ptr))						       \
		__stream->add(__stream,					       \
			      "Asserted " #ptr " is not error, but is: %ld",   \
			      strerror_r(-PTR_ERR(__ptr), buf, sizeof(buf)));  \
									       \
	ASSERT_END(test, !IS_ERR_OR_NULL(__ptr), __stream);		       \
} while (0)

/**
 * ASSERT_SIGSEGV() - An assertion that @expr will cause a segfault.
 * @test: The test context object.
 * @expr: an arbitrary block of code.
 *
 * Sets an assertion that @expr, when evaluated, will cause a segfault.
 * Currently this assertion is only really useful for testing the KUnit
 * framework, as a segmentation fault in normal kernel code is always incorrect.
 * However, the plan is to replace this assertion with an arbitrary death
 * assertion similar to
 * https://github.com/google/googletest/blob/master/googletest/docs/advanced.md#death-tests
 * which will probably be massaged to make sense in the context of the kernel
 * (maybe assert that a panic occurred, or that BUG() was called).
 *
 * NOTE: no code after this assertion will ever be executed.
 */
#define ASSERT_SIGSEGV(test, expr) do {					       \
	test->death_test = true;					       \
	expr;								       \
	test->death_test = false;					       \
	ASSERT_FAILURE(test,						       \
		       "Asserted that " #expr " would cause death, but did not.");\
} while (0)

/*
 * separate wrapper macro and functions to support 5.10 Kunit
 */
#include <kunit/test_wrapper.h>
#endif /* _TEST_TEST_H */
