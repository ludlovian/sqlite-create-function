.mode list
SELECT '--- 1. Load extension ---';

.load ./create_function

SELECT '--- 2. Define mul2 ---';
SELECT create_function('mul2', '?1 * 2');
SELECT mul2(5) as five_times_two, 10;

SELECT '--- 3. Multiple args ---';
SELECT create_function('power_sum', '(?1 * ?1) + (?2 * ?2)');
SELECT power_sum(3, 4) as pythagoras, 25;

SELECT '--- 4. Error - redefine ---';
SELECT create_function('mul2', '?1 * 2');

SELECT '--- 5. Cleanup ---';
SELECT create_function_clear();

SELECT '--- 6. Error ---';
SELECT mul2(5);

SELECT '--- 7. Second cleanup ---';
SELECT create_function_clear();

