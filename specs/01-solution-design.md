# problem: I want a javascript library to compare two CSV files and compute the differences, and second component that shows the computed differences on a web UI such as a ReactJS application. Since this is a library to be used in other web applications, the user of the library needs to be able to supply the stylesheet for the rendering.

## Business scenario:
   - A web application has a CSV stored in its database. A user downloads it and makes modification and tries to submit to the application. User should be shown a preview of the differences before the upload (in this case the source CSV needs to be downloaded from the server). If there is an approver, the approver would possibly do the comparison on the server side as both versions are avilable on the server and possibly in the database. So the application might use SQL based comparison. Alternately, a app server based comparison, in which case the library can be used as is depending on the programming language choice you will make (a server side library with necessary bindings for Javascript, Java, or Python may also be added to the scope)

## Background:
- These CSVs represent data in a relational database table. One or more columns may be key columns, if there are no key columns, then it is a all-keys row. 
- There should be provision for allowing 4 header rows in the CSV:
   + Row 1: If a cell in this row has the word KEY in it, it means that column participates in primary key 
   + Row 2: If a cell in this row has the word REQUIRED in it, it means the column cannot have nulls
   + Row 3: This row may have data type values, like : VARCHAR(30), DECIMAL(10,2), BOOLEAN, CHAR(5). 
   + Row 4: Each cell in this row has the table column names
   + Rest of the rows contain data in them

## Questions that I need you to help me to answer:
- How CSVs are supplied to the library?  - The library needs a way to accept the CSV data sources. The client UI is responsible source the CSV from the user using a file upload button, or a GET API call. What is the interface of the library? Does it receive file handles? Javascript collections? Callbacks that result in the CSV text stream? - Help me design the interface

- What programming language to use: The primary use case is CSV comparison on the browser, so Should the library be written in Javascript? Or, should it be written in C and WASM code generated? If the library needs to be usable in both server side as well as client side, the solution should allow it.
   + The reason for asking this question is that the CSV files could be as small as 1 KB or as big as 50 MB. It is extremely important to give fast response to users. If the diff processing takes 30 seconds or 2 minutes the user experience suffers and brings negative reviews from customers

- If WASM is the choice, I perfer to use C. You are free to choose Rust or Go. I have Go installed,but Rust is not installed on this machine yet.

- Data structure design? Is it better to use a Array of structs or Struct of Arrays? - I heard Struct of Arrays is better for reducing cache misses, and also possibly use allow SIMD assembly instructions and vectorization of data

- What if the CSV has multiline data enclosed in double quotes? How does the CSV parser deal with it? Will the library use an existing CSV parser library written in the language you will choose? 

- What if some cells have double quotes or single quotes in them, and the contents of the cells are enclosed in double quotes and the double quotes in the contents are escaped? How will the diff engine deal with this situation?

##  Constraints
1. The difference report should be shown in the order of the rows present in the modified CSV
	- What if the user re-orders rows from the source CSV? How does the library handle this?
	- What if the user re-orders the columns? The library should have a setting whether to allow column re-ordering or not so that it can be a input validation. Usually column validation should not be allowed as the data needs to go into a backing database table.


## Report UI needs

- User(The developer who is using the library and building their own UI) may want to show only the differing rows, and optionally allow the web page user to check a checkbox to show all rows
- The UI may want to show old value of a cel in the same cell by showing the new value in differnt color and bold and old value in gray and strike through above it or below it? 
- What are the options to show word or letter differences in a cell that has a multi word phrase like "Accident violation code" changed to "Accident Violation code(s)"? will the entire sentence be highlighted or the exact words or characters can be highlighted? THe view component should be able to support this need and accept a parameter to choose this option

- The user may want to show two rows of the csv one below the other and show the new row on top and old row below it. The matching values in the old row may be blank, the differing values may be shown below the corresponding cell

- Should the view component be a web component on its own? How to support table headers, column resizing, scrollbars? I would like those features to be added in incremental versions of the code

- How to allow client to supply their color scheme, font families, text sizes, etc. Can the library accept a configuration javascript object in which all sorts of settings are included?


Now that you read all my thoughts, write a document sharing your thoughts, solution  proposal, and design choices, and technical choice proposals or alternatives
-
- 
