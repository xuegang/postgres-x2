--
-- INT2
--

CREATE TABLE INT2_TBL(f1 int2);

INSERT INTO INT2_TBL(f1) VALUES ('0   ');

INSERT INTO INT2_TBL(f1) VALUES ('  1234 ');

INSERT INTO INT2_TBL(f1) VALUES ('    -1234');

INSERT INTO INT2_TBL(f1) VALUES ('34.5');

-- largest and smallest values
INSERT INTO INT2_TBL(f1) VALUES ('32767');

INSERT INTO INT2_TBL(f1) VALUES ('-32767');

-- bad input values -- should give errors
INSERT INTO INT2_TBL(f1) VALUES ('100000');
INSERT INTO INT2_TBL(f1) VALUES ('asdf');
INSERT INTO INT2_TBL(f1) VALUES ('    ');
INSERT INTO INT2_TBL(f1) VALUES ('- 1234');
INSERT INTO INT2_TBL(f1) VALUES ('4 444');
INSERT INTO INT2_TBL(f1) VALUES ('123 dt');
INSERT INTO INT2_TBL(f1) VALUES ('');


SELECT '' AS five, * FROM INT2_TBL ORDER BY f1;

SELECT '' AS four, i.* FROM INT2_TBL i WHERE i.f1 <> int2 '0' ORDER BY f1;

SELECT '' AS four, i.* FROM INT2_TBL i WHERE i.f1 <> int4 '0' ORDER BY f1;

SELECT '' AS one, i.* FROM INT2_TBL i WHERE i.f1 = int2 '0' ORDER BY f1;

SELECT '' AS one, i.* FROM INT2_TBL i WHERE i.f1 = int4 '0' ORDER BY f1;

SELECT '' AS two, i.* FROM INT2_TBL i WHERE i.f1 < int2 '0' ORDER BY f1;

SELECT '' AS two, i.* FROM INT2_TBL i WHERE i.f1 < int4 '0' ORDER BY f1;

SELECT '' AS three, i.* FROM INT2_TBL i WHERE i.f1 <= int2 '0' ORDER BY f1;

SELECT '' AS three, i.* FROM INT2_TBL i WHERE i.f1 <= int4 '0' ORDER BY f1;

SELECT '' AS two, i.* FROM INT2_TBL i WHERE i.f1 > int2 '0' ORDER BY f1;

SELECT '' AS two, i.* FROM INT2_TBL i WHERE i.f1 > int4 '0' ORDER BY f1;

SELECT '' AS three, i.* FROM INT2_TBL i WHERE i.f1 >= int2 '0' ORDER BY f1;

SELECT '' AS three, i.* FROM INT2_TBL i WHERE i.f1 >= int4 '0' ORDER BY f1;

-- positive odds 
SELECT '' AS one, i.* FROM INT2_TBL i WHERE (i.f1 % int2 '2') = int2 '1' ORDER BY f1;

-- any evens 
SELECT '' AS three, i.* FROM INT2_TBL i WHERE (i.f1 % int4 '2') = int2 '0' ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 * int2 '2' AS x FROM INT2_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 * int2 '2' AS x FROM INT2_TBL i
WHERE abs(f1) < 16384 ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 * int4 '2' AS x FROM INT2_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 + int2 '2' AS x FROM INT2_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 + int2 '2' AS x FROM INT2_TBL i
WHERE f1 < 32766 ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 + int4 '2' AS x FROM INT2_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 - int2 '2' AS x FROM INT2_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 - int2 '2' AS x FROM INT2_TBL i
WHERE f1 > -32767 ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 - int4 '2' AS x FROM INT2_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 / int2 '2' AS x FROM INT2_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 / int4 '2' AS x FROM INT2_TBL i ORDER BY f1;

-- corner cases
SELECT (-1::int2<<15)::text;
SELECT ((-1::int2<<15)+1::int2)::text;
