.mode list
SELECT 10, 'Load extension';

.load ./create_function

SELECT 20, 'Create new function';
SELECT 21, coalesce(create_function('mul2'), 'does not exist');
SELECT 22, create_function('mul2', 'select ?1 * 2');
SELECT 23, create_function('mul2');
SELECT 24, mul2(5);

SELECT 30, 'Multiple args';
SELECT 31, create_function('power_sum', 'select (?1 * ?1) + (?2 * ?2)');
SELECT 32, create_function('power_sum');
SELECT 33, power_sum(3,4);
SELECT 34, power_sum(mul2(3), mul2(4));

SELECT 40, 'Redefine';
SELECT 41, create_function('mul2', 'select 2 * ?');
SELECT 42, create_function('mul2');

SELECT 50, 'Clear';
SELECT 51, create_function_clear();

SELECT 60, 'Inpsect after clear';
SELECT 61, create_function('mul2');
SELECT 62, 'Redefine after clear';
SELECT 63, create_function('mul2', 'select ?1 + ?1');

SELECT 70, 'Non readonly';
SELECT 71, create_function('evil','create table temp.t1(x)');

SELECT 80, 'Second clear';
SELECT 81, create_function_clear();

SELECT 90, 'new func';
SELECT 91, create_function('triple', 'select ? * 3');
SELECT 92, triple(3);

SELECT 100, 'final clear';
SELECT 101, create_function_clear();
