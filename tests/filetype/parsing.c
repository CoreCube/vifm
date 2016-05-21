#include <stic.h>

#include "../../src/utils/string_array.h"
#include "../../src/filetype.h"

TEST(empty_input)
{
	char **list;
	int count;

	list = break_into_matchers("", &count);
	assert_int_equal(1, count);
	if(count == 1)
	{
		assert_string_equal("", list[0]);
	}

	free_string_array(list, count);
}

TEST(extra_emark_breaks_everything)
{
	char **list;
	int count;

	list = break_into_matchers("{ng1,ng2}!!{ng3}{{pg1,pg2}}", &count);
	assert_int_equal(1, count);
	if(count == 1)
	{
		assert_string_equal("{ng1,ng2}!!{ng3}{{pg1,pg2}}", list[0]);
	}

	free_string_array(list, count);
}

TEST(globs_are_parsed_well)
{
	char **list;
	int count;

	list = break_into_matchers("{ng1,{ng2}!{ng3}{{pg1,pg2}}!{{pg3}}", &count);
	assert_int_equal(4, count);
	if(count == 4)
	{
		assert_string_equal("{ng1,{ng2}", list[0]);
		assert_string_equal("!{ng3}", list[1]);
		assert_string_equal("{{pg1,pg2}}", list[2]);
		assert_string_equal("!{{pg3}}", list[3]);
	}

	free_string_array(list, count);
}

TEST(regexps_are_parsed_well)
{
	char **list;
	int count;

	list = break_into_matchers("!/nr1//nr2///pr1////pr2//", &count);
	assert_int_equal(4, count);
	if(count == 4)
	{
		assert_string_equal("!/nr1/", list[0]);
		assert_string_equal("/nr2/", list[1]);
		assert_string_equal("//pr1//", list[2]);
		assert_string_equal("//pr2//", list[3]);
	}

	free_string_array(list, count);
}

TEST(regexps_escaping_works)
{
	char **list;
	int count;

	list = break_into_matchers("!/n\\/r1//nr2///p\\/\\/r1////pr/\\//2//", &count);
	assert_int_equal(4, count);
	if(count == 4)
	{
		assert_string_equal("!/n\\/r1/", list[0]);
		assert_string_equal("/nr2/", list[1]);
		assert_string_equal("//p\\/\\/r1//", list[2]);
		assert_string_equal("//pr/\\//2//", list[3]);
	}

	free_string_array(list, count);
}

TEST(regexs_with_correct_flags)
{
	char **list;
	int count;

	list = break_into_matchers("!/nr1/iIiiiII//asdf//iI", &count);
	assert_int_equal(2, count);
	if(count == 2)
	{
		assert_string_equal("!/nr1/iIiiiII", list[0]);
		assert_string_equal("//asdf//iI", list[1]);
	}

	free_string_array(list, count);
}

TEST(regexs_with_incorrect_flags)
{
	char **list;
	int count;

	list = break_into_matchers("//a//!/nr1/a", &count);
	assert_int_equal(1, count);
	if(count == 1)
	{
		assert_string_equal("//a//!/nr1/a", list[0]);
	}

	free_string_array(list, count);
}

TEST(mime_patterns)
{
	char **list;
	int count;

	list = break_into_matchers("<a><<<<>!<b>", &count);
	assert_int_equal(3, count);
	if(count == 3)
	{
		assert_string_equal("<a>", list[0]);
		assert_string_equal("<<<<>", list[1]);
		assert_string_equal("!<b>", list[2]);
	}

	free_string_array(list, count);
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
