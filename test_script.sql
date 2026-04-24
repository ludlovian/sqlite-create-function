.mode list
.echo on

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

SELECT 40, 'Caching';
SELECT 41, create_function(NULL, 'cache');
SELECT 42, power_sum(mul2(3), mul2(4));
SELECT 43, create_function(NULL, 'cAcHe');
SELECT 44, power_sum(mul2(3), mul2(4));

SELECT 50, 'Clearing';
SELECT 51, create_function(NULL, 'clear');
SELECT 52, power_sum(mul2(3), mul2(4));
SELECT 53, create_function(NULL, 'cLeAr');

SELECT 60, 'Errors';
SELECT 61, create_function('mul2', 'select ?1 + ?1');
SELECT 62, create_function('mul3', 'triple');
SELECT 63, create_function(NULL, 'badcmd');
SELECT 64, create_function('err', 4);
SELECT 65, create_function(4, 'err');
SELECT 66, create_function('evil','create table temp.t1(x)');
SELECT 67, create_function('twubble', 'select ?1, ?1');

SELECT 99, create_function(NULL, 'CLEAR');

